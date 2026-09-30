/**
 * @file Service.hpp
 * @brief Точка подмены транспорта и доступ к настройкам модуля Xiaomi.
 *
 * Повторяет форму тест-швов Billing и Storage: боевой синглтон по умолчанию,
 * install_for_testing подменяет его на подделку. Контроллер берёт транспорт
 * здесь и не знает, настоящий он или тестовый.
 */

#pragma once

#include <string>

#include "xiaomi/HttpTransport.hpp"

namespace Xiaomi::Service {

/// Транспорт до облака: CurlTransport, пока тест не подменил его.
HttpTransport& transport();

/// Подменить транспорт в тестах. nullptr возвращает боевой.
void install_for_testing(HttpTransport* transport);

/// Ключ шифрования токена из конфига (xiaomi.token_key / MI_FITNESS_TOKEN_KEY).
/// Пустая строка означает «не настроено», решает вызывающий.
std::string token_key_b64();

/// Предел одного HTTP-запроса к облаку в секундах
/// (xiaomi.http_timeout_seconds / MI_FITNESS_HTTP_TIMEOUT, дефолт 20).
/// Глубокий бэкфил облако отдаёт дольше 20 секунд, ручка поднимает предел
/// без пересборки. До инициализации конфига возвращает дефолт.
long http_timeout_seconds();

}  // namespace Xiaomi::Service
