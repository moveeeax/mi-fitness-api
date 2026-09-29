/**
 * @file CloudClient.cpp
 * @brief Тела клиента облака: вход и проверка адреса редиректа.
 */

#include "xiaomi/CloudClient.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "xiaomi/Crypto.hpp"
#include "xiaomi/Regions.hpp"

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

namespace {

/// Процентное кодирование для application/x-www-form-urlencoded: всё, кроме
/// букв, цифр и -_.~. Пробел кодируется как %20, а не плюсом: так делает
/// urlencode в Python с настройками апстрима.
std::string url_encode(std::string_view value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        const auto byte = static_cast<unsigned char>(c);
        const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                                (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' ||
                                byte == '~';
        if (unreserved) {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0x0f]);
        }
    }
    return out;
}

/// Коды авторизации апстрима. Такой отказ не ретраится: повтор только сжигает
/// попытки, нужен свежий токен.
bool is_authentication_code(long long code) {
    return code == 401 || code == 403 || code == -6 || code == -10001;
}

}  // namespace

nlohmann::json CloudClient::post_signed(const std::string& base_url,
                                        std::string_view api_path,
                                        const nlohmann::json& payload) {
    if (ssecurity_b64_.empty() || cookies_.empty()) {
        throw MiFitnessAuthError("data request before a successful login");
    }

    const std::string data = payload.dump();
    const auto minutes =
        std::chrono::duration_cast<std::chrono::minutes>(std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string nonce = Crypto::make_nonce(minutes, Crypto::random_bytes(8));
    const std::string signed_nonce = Crypto::signed_nonce(ssecurity_b64_, nonce);

    // Подпись считается дважды: до шифрования от открытой формы и после от
    // зашифрованных значений. Порядок обязателен, он проверен золотыми
    // векторами.
    const std::string rc4_hash = Crypto::signature("POST", api_path, data, std::nullopt, signed_nonce);
    const std::string encrypted_data = Crypto::b64_encode(Crypto::rc4(signed_nonce, data));
    const std::string encrypted_hash = Crypto::b64_encode(Crypto::rc4(signed_nonce, rc4_hash));
    const std::string signature = Crypto::signature("POST", api_path, encrypted_data, encrypted_hash, signed_nonce);

    HttpRequest request;
    request.method = "POST";
    request.url = base_url + std::string(api_path);
    request.body = "data=" + url_encode(encrypted_data) + "&rc4_hash__=" + url_encode(encrypted_hash) +
                   "&signature=" + url_encode(signature) + "&_nonce=" + url_encode(Crypto::b64_encode(nonce));
    request.headers.emplace_back("Cookie", cookies_);
    request.headers.emplace_back("Content-Type", "application/x-www-form-urlencoded");

    const HttpResponse response = transport_.send(request);
    if (response.status == 401 || response.status == 403) {
        throw MiFitnessAuthError("Xiaomi data request was refused: HTTP " + std::to_string(response.status));
    }
    if (response.status != 200) {
        throw MiFitnessProtocolError("Xiaomi data request failed: HTTP " + std::to_string(response.status));
    }

    const std::string plaintext = Crypto::rc4(signed_nonce, Crypto::b64_decode(response.body));
    const nlohmann::json envelope = nlohmann::json::parse(plaintext, nullptr, /*allow_exceptions=*/false);
    if (envelope.is_discarded() || !envelope.is_object()) {
        // Первый признак смены закрытого формата: расшифровалось, но это не
        // JSON, либо не расшифровалось вовсе.
        throw MiFitnessProtocolError("Xiaomi response did not decrypt to a JSON object");
    }

    const long long code =
        envelope.contains("code") && envelope["code"].is_number_integer() ? envelope["code"].get<long long>() : -1;
    if (code != 0) {
        // Текст message управляется сервером и в ошибку не идёт: код достаточен
        // для диагностики, а содержимое чужой строки в логах не нужно.
        if (is_authentication_code(code)) {
            throw MiFitnessAuthError("Xiaomi refused authentication, code " + std::to_string(code));
        }
        throw MiFitnessProtocolError("Xiaomi returned error code " + std::to_string(code));
    }
    return envelope.value("result", nlohmann::json::object());
}

std::vector<nlohmann::json> CloudClient::fetch_key(std::string_view key,
                                                   std::string_view start_date,
                                                   std::string_view end_date,
                                                   std::optional<std::string_view> region) {
    const std::string region_name(region.value_or(std::string_view(credentials_.region)));
    const std::string base_url = host_for_region(region_name);
    const auto [start_time, end_time] = range_to_timestamps(start_date, end_date, region_name);

    std::vector<nlohmann::json> items;
    std::set<std::string> seen_cursors;
    std::optional<std::string> next_key;
    for (int page = 1;; ++page) {
        if (page > max_pages_) {
            throw MiFitnessProtocolError("Xiaomi pagination exceeded the page ceiling");
        }
        nlohmann::json payload{{"start_time", start_time}, {"end_time", end_time}, {"key", std::string(key)}};
        if (next_key.has_value()) {
            payload["next_key"] = *next_key;
        }
        const nlohmann::json result = post_signed(base_url, "/app/v1/data/get_fitness_data_by_time", payload);

        if (result.contains("data_list") && result["data_list"].is_array()) {
            for (const auto& item : result["data_list"]) {
                items.push_back(item);
            }
        }
        if (!result.value("has_more", false) || !result.contains("next_key") || result["next_key"].is_null()) {
            break;
        }
        const auto& cursor_json = result["next_key"];
        const std::string cursor = cursor_json.is_string() ? cursor_json.get<std::string>() : cursor_json.dump();
        // Повтор курсора это петля, и лучше упасть, чем крутиться вечно.
        if (!seen_cursors.insert(cursor).second) {
            throw MiFitnessProtocolError("Xiaomi pagination cursor loop detected");
        }
        next_key = cursor;
    }
    return items;
}

}  // namespace Xiaomi
