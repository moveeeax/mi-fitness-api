/**
 * @file CurlTransport.hpp
 * @brief Боевой транспорт на libcurl.
 *
 * Тонкий адаптер без логики: вся проверка ответов живёт в CloudClient и
 * покрыта тестами на подделке. Здесь проверять нечего, кроме настроек curl,
 * и они подтверждаются живым запросом при выкатке.
 *
 * Редиректам не следует намеренно: цель проверяет клиент. Автоматический
 * переход отправил бы куки сессии куда угодно.
 */

#pragma once

#include <string>

#include "xiaomi/HttpTransport.hpp"

namespace Xiaomi {

class CurlTransport : public HttpTransport {
public:
    explicit CurlTransport(long timeout_seconds = 20) : timeout_seconds_(timeout_seconds) {}

    HttpResponse send(const HttpRequest& request) override;

    /// Предел одного запроса в секундах: тестам и диагностике.
    long timeout_seconds() const { return timeout_seconds_; }

private:
    long timeout_seconds_;
};

}  // namespace Xiaomi
