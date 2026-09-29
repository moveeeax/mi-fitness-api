/**
 * @file CloudClient.cpp
 * @brief Тела клиента облака: вход и проверка адреса редиректа.
 */

#include "xiaomi/CloudClient.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "xiaomi/Crypto.hpp"

namespace Xiaomi {

namespace {

constexpr std::string_view kLoginPrefix = "&&&START&&&";
constexpr std::string_view kLoginUrl = "https://account.xiaomi.com/pass/serviceLogin?_json=true&sid=miothealth";

bool host_is_xiaomi(const std::string& host) {
    static constexpr std::string_view kDomains[] = {"xiaomi.com", "mi.com"};
    for (const auto& domain : kDomains) {
        if (host == domain) {
            return true;
        }
        if (host.size() > domain.size() + 1 && host.compare(host.size() - domain.size(), domain.size(), domain) == 0 &&
            host[host.size() - domain.size() - 1] == '.') {
            return true;
        }
    }
    return false;
}

std::string lowercase(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

/// Значение заголовка без учёта регистра имени. Возвращает все совпадения:
/// Set-Cookie приходит несколько раз.
std::vector<std::string> header_values(const Headers& headers, std::string_view name) {
    std::vector<std::string> out;
    const std::string wanted = lowercase(std::string(name));
    for (const auto& [key, value] : headers) {
        if (lowercase(key) == wanted) {
            out.push_back(value);
        }
    }
    return out;
}

/// Значение поля, которое Xiaomi отдаёт то строкой, то числом (userId).
std::string field_as_string(const nlohmann::json& payload, const char* key) {
    const auto& value = payload.at(key);
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<long long>());
    }
    throw MiFitnessAuthError(std::string("Xiaomi login response field has an unexpected type: ") + key);
}

}  // namespace

bool is_allowed_login_redirect(std::string_view url) {
    if (url.empty()) {
        return false;
    }
    for (const char c : url) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte == 0x7f) {
            return false;  // пробелы и управляющие символы
        }
    }

    const auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos || url.substr(0, scheme_end) != "https") {
        return false;
    }

    const std::string_view rest = url.substr(scheme_end + 3);
    const auto authority_end = rest.find_first_of("/?#");
    const std::string_view authority = authority_end == std::string_view::npos ? rest : rest.substr(0, authority_end);
    if (authority.empty()) {
        return false;
    }
    // Учётные данные в адресе запрещены: с ними цель редиректа перестаёт быть
    // тем, чем выглядит.
    if (authority.find('@') != std::string_view::npos) {
        return false;
    }

    std::string host(authority);
    const auto colon = host.find(':');
    if (colon != std::string::npos) {
        if (host.substr(colon + 1) != "443") {
            return false;
        }
        host.erase(colon);
    }
    return host_is_xiaomi(lowercase(host));
}

CloudClient::CloudClient(HttpTransport& transport,
                         Credentials credentials,
                         std::function<void(const Credentials&)> on_rotate)
    : transport_(transport), credentials_(std::move(credentials)), on_rotate_(std::move(on_rotate)) {}

void CloudClient::login() {
    validate_pass_token(credentials_.pass_token);

    HttpRequest request;
    request.method = "GET";
    request.url = std::string(kLoginUrl);
    request.headers.emplace_back("Cookie", "userId=" + credentials_.user_id + "; passToken=" + credentials_.pass_token);

    const HttpResponse response = transport_.send(request);
    if (response.body.rfind(kLoginPrefix, 0) != 0) {
        // Тело в текст ошибки не идёт: там бывают куски учётных данных.
        throw MiFitnessAuthError("Xiaomi login response is missing the &&&START&&& prefix");
    }

    nlohmann::json payload;
    {
        const std::string raw = response.body.substr(kLoginPrefix.size());
        payload = nlohmann::json::parse(raw, nullptr, /*allow_exceptions=*/false);
        // Разбор без исключений намеренно: сообщение nlohmann содержит кусок
        // входа, а вход это ответ логина.
        if (payload.is_discarded() || !payload.is_object()) {
            throw MiFitnessAuthError("Xiaomi login response is not a JSON object");
        }
    }

    for (const char* key : {"passToken", "userId", "ssecurity", "location"}) {
        if (!payload.contains(key) || payload.at(key).is_null()) {
            throw MiFitnessAuthError(std::string("Xiaomi login response is missing a field: ") + key);
        }
    }

    const std::string rotated_token = field_as_string(payload, "passToken");
    const std::string user_id = field_as_string(payload, "userId");
    const std::string ssecurity = field_as_string(payload, "ssecurity");
    const std::string location = field_as_string(payload, "location");

    validate_pass_token(rotated_token);
    validate_pass_token(user_id);  // те же ограничения: значение уходит в Cookie
    // Бросает, если это не base64: без ssecurity подписать запрос нельзя, а
    // молча хранить мусор значит отложить отказ до первого запроса данных.
    static_cast<void>(Crypto::b64_decode(ssecurity));

    if (!is_allowed_login_redirect(location)) {
        throw MiFitnessAuthError("Xiaomi login redirect points outside the allowed hosts");
    }

    // Состояние обновляется до запроса по редиректу: Xiaomi уже выдал новый
    // токен, и сетевая ошибка на следующем шаге не должна его потерять.
    const bool rotated = rotated_token != credentials_.pass_token;
    credentials_.pass_token = rotated_token;
    credentials_.user_id = user_id;
    ssecurity_b64_ = ssecurity;

    if (rotated && on_rotate_) {
        try {
            on_rotate_(credentials_);
        } catch (const std::exception& e) {
            // Записать не удалось, но токен уже выдан и работает в этой сессии.
            // Падать здесь значит потерять его совсем.
            spdlog::warn("failed to persist the rotated Xiaomi passToken: {}", e.what());
        }
    }

    HttpRequest redirect;
    redirect.method = "GET";
    redirect.url = location;
    const HttpResponse redirect_response = transport_.send(redirect);

    std::string cookies;
    bool has_service_token = false;
    for (const auto& value : header_values(redirect_response.headers, "set-cookie")) {
        const std::string pair = value.substr(0, value.find(';'));
        if (pair.rfind("serviceToken=", 0) == 0 && pair.size() > std::string("serviceToken=").size()) {
            has_service_token = true;
        }
        if (!cookies.empty()) {
            cookies += "; ";
        }
        cookies += pair;
    }
    if (!has_service_token) {
        throw MiFitnessAuthError("Xiaomi login response is missing a serviceToken cookie");
    }
    cookies_ = cookies;
}

}  // namespace Xiaomi
