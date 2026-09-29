/**
 * @file HttpTransport.hpp
 * @brief Шов между клиентом облака и сетью.
 *
 * Клиент говорит только с этим интерфейсом, поэтому весь разбор ответов Xiaomi
 * проверяется без сети. В боевом коде за интерфейсом стоит CurlTransport, в
 * тестах подделка с заготовленными ответами.
 */

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace Xiaomi {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct HttpRequest {
    std::string method;
    std::string url;
    std::string body;
    Headers headers;
};

struct HttpResponse {
    long status = 0;
    std::string body;
    /// Имена приходят как есть от сервера, поэтому сравнивать их надо без учёта
    /// регистра. Список, а не словарь: Set-Cookie повторяется.
    Headers headers;
};

class HttpTransport {
public:
    HttpTransport() = default;
    HttpTransport(const HttpTransport&) = delete;
    HttpTransport& operator=(const HttpTransport&) = delete;
    HttpTransport(HttpTransport&&) = delete;
    HttpTransport& operator=(HttpTransport&&) = delete;
    virtual ~HttpTransport() = default;

    /// Редиректам следовать не должен: клиент проверяет цель сам.
    virtual HttpResponse send(const HttpRequest& request) = 0;
};

}  // namespace Xiaomi
