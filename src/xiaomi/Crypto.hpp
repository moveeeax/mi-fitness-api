/**
 * @file Crypto.hpp
 * @brief Крипта закрытого протокола облака Mi Fitness: RC4, nonce, подпись.
 *
 * Чистые функции без сети и без состояния. Соответствие апстриму проверяется
 * золотыми векторами (tests/fixtures/xiaomi_crypto_vectors.json), снятыми с
 * Python-реализации: рассуждением такой алгоритм не проверить.
 *
 * Объявления только на std-типах, тела в Crypto.cpp, чтобы заголовки OpenSSL
 * не протекали в каждую единицу трансляции (та же причина, что у
 * utils/Crypto.hpp).
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Xiaomi::Crypto {

/// Стандартный base64 с паддингом. Utils::Base64 здесь не годится: там
/// base64url без паддинга, а протокол Xiaomi требует обычный алфавит.
std::string b64_encode(std::string_view raw);

/// Бросает MiFitnessAuthError на любом входе, который не является корректным
/// стандартным base64: молча вернуть мусор здесь опаснее, чем упасть.
std::string b64_decode(std::string_view encoded);

/**
 * @brief RC4 в варианте Xiaomi: после инициализации ключа отбрасывается 1024
 *        байта потока.
 *
 * Шифрование и расшифровка это одна и та же операция. Пустой ключ отвергается:
 * в Python он давал исключение при делении по модулю нуля, в C++ это было бы
 * неопределённое поведение.
 */
std::string rc4(std::string_view key, std::string_view payload);

/// SHA256(ssecurity || nonce), где ssecurity приходит в base64, а nonce сырыми
/// байтами. Возвращает 32 сырых байта.
std::string signed_nonce(std::string_view ssecurity_b64, std::string_view nonce_raw);

/// 8 байт случайности плюс 4 байта big-endian с номером минуты от эпохи.
/// Ровно 12 байт: длину случайной части проверяем, чтобы не собрать короткий
/// nonce, который облако молча отвергнет.
std::string make_nonce(std::int64_t minutes_since_epoch, std::string_view random8);

/// 8 байт случайности из OpenSSL для передачи в make_nonce.
std::string random_bytes(std::size_t count);

/// base64(SHA1(method & path & data=<data> [& rc4_hash__=<hash>] & base64(signed_nonce))).
/// Порядок полей обязателен: подпись считается дважды, до и после шифрования,
/// и оба раза по этой же схеме.
std::string signature(std::string_view method,
                      std::string_view path,
                      std::string_view data,
                      std::optional<std::string_view> rc4_hash,
                      std::string_view signed_nonce_raw);

}  // namespace Xiaomi::Crypto
