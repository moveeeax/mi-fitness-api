/**
 * @file Credentials.hpp
 * @brief Учётные данные аккаунта Xiaomi: проверка токена, маскирование, шифрование.
 *
 * Xiaomi ротирует passToken при каждом логине, поэтому Secret кластера держит
 * только стартовое значение, а живое лежит в Postgres зашифрованным. Ключ
 * шифрования приходит из окружения и в базу не попадает.
 *
 * Объявления только на std-типах, тела в Credentials.cpp: заголовки libsodium
 * не должны протекать наружу (та же причина, что у utils/Crypto.hpp).
 */

#pragma once

#include <string>
#include <string_view>

#include "xiaomi/Errors.hpp"

namespace Xiaomi {

/// Учётные данные одного аккаунта. Регион нужен здесь же: он определяет и хост
/// облака, и часовой пояс, в котором считаются границы суток.
struct Credentials {
    std::string user_id;
    std::string pass_token;
    std::string region;
};

/// Результат шифрования: сырые байты, без кодирования. В base64 их переводит
/// репозиторий, когда кладёт в TEXT-колонки.
struct Sealed {
    std::string ciphertext;
    std::string nonce;
};

/**
 * @brief Отвергает токен, который нельзя положить в заголовок Cookie.
 *
 * Заголовок обязан быть ASCII без пробелов и без точки с запятой. Практический
 * случай: значение, скопированное из таблицы куки в DevTools, приезжает с U+2026
 * в середине, и без этой проверки отказ случается уже на сетевом запросе с
 * невнятной ошибкой кодирования. Сообщение называет позицию плохого символа и
 * никогда само значение: текст ошибки уезжает в логи.
 */
void validate_pass_token(std::string_view token);

/// Шесть звёзд и два последних символа. Длина исходного значения по маске не
/// восстанавливается. Пустой вход даёт пустую строку.
std::string mask_account_id(std::string_view value);

/// crypto_secretbox_easy со свежим nonce. Ключ приходит в base64 и обязан быть
/// длиной crypto_secretbox_KEYBYTES, иначе MiFitnessAuthError.
Sealed seal(std::string_view plain, std::string_view key_b64);

/// Обратная операция. Неверный ключ и любая подмена шифротекста дают
/// MiFitnessAuthError без подробностей: детали тут ничем не помогают, а в лог
/// попадают.
std::string unseal(const Sealed& sealed, std::string_view key_b64);

}  // namespace Xiaomi
