/**
 * @file CurlTransport.cpp
 * @brief Тело боевого транспорта.
 */

#include "xiaomi/CurlTransport.hpp"

#include <cstddef>
#include <memory>
#include <string>

#include <curl/curl.h>

#include "utils/CurlInit.hpp"
#include "xiaomi/Errors.hpp"

namespace Xiaomi {

namespace {

std::size_t append_body(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

std::size_t append_header(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* out = static_cast<Headers*>(userdata);
    const std::string line(ptr, size * nmemb);
    const auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        const auto first = value.find_first_not_of(" \t");
        const auto last = value.find_last_not_of("\r\n \t");
        value = (first == std::string::npos) ? std::string() : value.substr(first, last - first + 1);
        out->emplace_back(std::move(name), std::move(value));
    }
    return size * nmemb;
}

}  // namespace

HttpResponse CurlTransport::send(const HttpRequest& request) {
    Utils::ensure_curl_init();

    std::unique_ptr<CURL, void (*)(CURL*)> handle(::curl_easy_init(), ::curl_easy_cleanup);
    if (!handle) {
        throw MiFitnessProtocolError("curl_easy_init failed");
    }

    HttpResponse response;
    ::curl_slist* headers = nullptr;
    for (const auto& [name, value] : request.headers) {
        headers = ::curl_slist_append(headers, (name + ": " + value).c_str());
    }
    const std::unique_ptr<::curl_slist, void (*)(::curl_slist*)> header_guard(headers, ::curl_slist_free_all);

    ::curl_easy_setopt(handle.get(), CURLOPT_URL, request.url.c_str());
    ::curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, append_body);
    ::curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &response.body);
    ::curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, append_header);
    ::curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &response.headers);
    ::curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT, timeout_seconds_);
    ::curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
    // Не следовать редиректу: цель проверяет клиент, иначе куки сессии уедут
    // туда, куда укажет ответ.
    ::curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0L);
    if (headers != nullptr) {
        ::curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers);
    }
    if (request.method == "POST") {
        ::curl_easy_setopt(handle.get(), CURLOPT_POST, 1L);
        ::curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, request.body.c_str());
        ::curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
    }

    const CURLcode rc = ::curl_easy_perform(handle.get());
    if (rc != CURLE_OK) {
        // Текст curl безопасен: адрес в нём не появляется, а он может нести
        // строку запроса с учётными данными.
        throw MiFitnessProtocolError(std::string("Xiaomi request failed: ") + ::curl_easy_strerror(rc));
    }
    ::curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &response.status);
    return response;
}

}  // namespace Xiaomi
