/**
 * @file Credentials.cpp
 * @brief Тела проверки, маскирования и шифрования учётных данных Xiaomi.
 */

#include "xiaomi/Credentials.hpp"

#include <cstddef>
#include <string>
#include <string_view>

#include <sodium.h>

#include "xiaomi/Crypto.hpp"

namespace Xiaomi {

namespace {

/// Идемпотентная инициализация libsodium. Повторяет приём из
/// security/Password.hpp намеренно: тащить ребро xiaomi -> security ради трёх
/// строк значит связать модули там, где связи нет.
void ensure_sodium_initialized() {
    static const int rc = ::sodium_init();  // 0 при первой инициализации, 1 если уже была
    if (rc < 0) {
        throw MiFitnessAuthError("libsodium failed to initialize");
    }
}

std::string decode_key(std::string_view key_b64) {
    const std::string key = Crypto::b64_decode(key_b64);
    if (key.size() != crypto_secretbox_KEYBYTES) {
        throw MiFitnessAuthError("token key must decode to exactly " + std::to_string(crypto_secretbox_KEYBYTES) +
                                 " bytes");
    }
    return key;
}

const unsigned char* as_bytes(const std::string& s) {
    return reinterpret_cast<const unsigned char*>(s.data());
}

unsigned char* as_writable_bytes(std::string& s) {
    return reinterpret_cast<unsigned char*>(s.data());
}

}  // namespace

void validate_pass_token(std::string_view token) {
    if (token.empty()) {
        throw MiFitnessAuthError("passToken is empty");
    }
    for (std::size_t i = 0; i < token.size(); ++i) {
        const auto c = static_cast<unsigned char>(token[i]);
        // Допустимы только печатные ASCII без точки с запятой: 0x21 это '!',
        // всё ниже это пробел и управляющие символы, всё выше 0x7f не ASCII.
        if (c < 0x21 || c > 0x7f || c == ';') {
            throw MiFitnessAuthError("passToken has a character that cannot go into a Cookie header at position " +
                                     std::to_string(i));
        }
    }
}

std::string mask_account_id(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    // Ровно шесть звёзд независимо от длины: иначе маска выдаёт размер
    // идентификатора. Значения короче трёх символов закрываются целиком.
    if (value.size() <= 2) {
        return "******";
    }
    return "******" + std::string(value.substr(value.size() - 2));
}

Sealed seal(std::string_view plain, std::string_view key_b64) {
    ensure_sodium_initialized();
    const std::string key = decode_key(key_b64);

    Sealed out;
    out.nonce.resize(crypto_secretbox_NONCEBYTES);
    ::randombytes_buf(as_writable_bytes(out.nonce), out.nonce.size());

    out.ciphertext.resize(plain.size() + crypto_secretbox_MACBYTES);
    const int rc = ::crypto_secretbox_easy(as_writable_bytes(out.ciphertext),
                                           reinterpret_cast<const unsigned char*>(plain.data()),
                                           static_cast<unsigned long long>(plain.size()),
                                           as_bytes(out.nonce),
                                           as_bytes(key));
    if (rc != 0) {
        throw MiFitnessAuthError("sealing the passToken failed");
    }
    return out;
}

std::string unseal(const Sealed& sealed, std::string_view key_b64) {
    ensure_sodium_initialized();
    const std::string key = decode_key(key_b64);

    if (sealed.nonce.size() != crypto_secretbox_NONCEBYTES) {
        throw MiFitnessAuthError("stored nonce has the wrong length");
    }
    if (sealed.ciphertext.size() < crypto_secretbox_MACBYTES) {
        throw MiFitnessAuthError("stored ciphertext is shorter than the authentication tag");
    }

    std::string plain(sealed.ciphertext.size() - crypto_secretbox_MACBYTES, '\0');
    const int rc = ::crypto_secretbox_open_easy(plain.empty() ? nullptr : as_writable_bytes(plain),
                                                as_bytes(sealed.ciphertext),
                                                static_cast<unsigned long long>(sealed.ciphertext.size()),
                                                as_bytes(sealed.nonce),
                                                as_bytes(key));
    if (rc != 0) {
        // Подмена шифротекста и неверный ключ неразличимы снаружи намеренно.
        throw MiFitnessAuthError("stored passToken failed authentication (wrong key or tampered)");
    }
    return plain;
}

}  // namespace Xiaomi
