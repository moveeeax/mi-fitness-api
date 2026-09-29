/**
 * @file Crypto.cpp
 * @brief Тела крипты протокола Mi Fitness.
 */

#include "xiaomi/Crypto.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <openssl/rand.h>
#include <openssl/sha.h>

#include "xiaomi/Errors.hpp"

namespace Xiaomi::Crypto {

namespace {

constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// Обратная таблица алфавита: 0xff означает «символ не из base64».
std::array<unsigned char, 256> build_reverse_table() {
    std::array<unsigned char, 256> table{};
    table.fill(0xff);
    for (std::size_t i = 0; i < kAlphabet.size(); ++i) {
        table[static_cast<unsigned char>(kAlphabet[i])] = static_cast<unsigned char>(i);
    }
    return table;
}

unsigned char byte_at(std::string_view s, std::size_t i) {
    return static_cast<unsigned char>(s[i]);
}

}  // namespace

std::string b64_encode(std::string_view raw) {
    std::string out;
    out.reserve(((raw.size() + 2) / 3) * 4);

    std::size_t i = 0;
    while (i + 3 <= raw.size()) {
        const unsigned int triple = (static_cast<unsigned int>(byte_at(raw, i)) << 16) |
                                    (static_cast<unsigned int>(byte_at(raw, i + 1)) << 8) |
                                    static_cast<unsigned int>(byte_at(raw, i + 2));
        out.push_back(kAlphabet[(triple >> 18) & 0x3f]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3f]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3f]);
        out.push_back(kAlphabet[triple & 0x3f]);
        i += 3;
    }

    const std::size_t rest = raw.size() - i;
    if (rest == 1) {
        const unsigned int triple = static_cast<unsigned int>(byte_at(raw, i)) << 16;
        out.push_back(kAlphabet[(triple >> 18) & 0x3f]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3f]);
        out.append("==");
    } else if (rest == 2) {
        const unsigned int triple =
            (static_cast<unsigned int>(byte_at(raw, i)) << 16) | (static_cast<unsigned int>(byte_at(raw, i + 1)) << 8);
        out.push_back(kAlphabet[(triple >> 18) & 0x3f]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3f]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3f]);
        out.push_back('=');
    }
    return out;
}

std::string b64_decode(std::string_view encoded) {
    static const std::array<unsigned char, 256> reverse = build_reverse_table();

    if (encoded.size() % 4 != 0) {
        throw MiFitnessAuthError("base64: length is not a multiple of four");
    }
    if (encoded.empty()) {
        return {};
    }

    std::size_t padding = 0;
    if (encoded[encoded.size() - 1] == '=') {
        ++padding;
        if (encoded.size() >= 2 && encoded[encoded.size() - 2] == '=') {
            ++padding;
        }
    }

    std::string out;
    out.reserve((encoded.size() / 4) * 3);

    for (std::size_t i = 0; i < encoded.size(); i += 4) {
        unsigned int quad = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            const std::size_t at = i + k;
            const bool is_pad = encoded[at] == '=';
            // Паддинг разрешён только в самом хвосте: «=» в середине это мусор.
            if (is_pad && at + padding < encoded.size()) {
                throw MiFitnessAuthError("base64: padding in the middle of the input");
            }
            const unsigned char value = is_pad ? 0 : reverse[byte_at(encoded, at)];
            if (value == 0xff) {
                throw MiFitnessAuthError("base64: character outside the standard alphabet");
            }
            quad = (quad << 6) | value;
        }
        out.push_back(static_cast<char>((quad >> 16) & 0xff));
        out.push_back(static_cast<char>((quad >> 8) & 0xff));
        out.push_back(static_cast<char>(quad & 0xff));
    }

    out.resize(out.size() - padding);
    return out;
}

std::string rc4(std::string_view key, std::string_view payload) {
    if (key.empty()) {
        throw MiFitnessAuthError("rc4: empty key (missing or malformed ssecurity)");
    }

    std::array<unsigned char, 256> state{};
    for (std::size_t i = 0; i < state.size(); ++i) {
        state[i] = static_cast<unsigned char>(i);
    }
    std::size_t j = 0;
    for (std::size_t i = 0; i < state.size(); ++i) {
        j = (j + state[i] + byte_at(key, i % key.size())) % 256;
        std::swap(state[i], state[j]);
    }

    std::size_t x = 0;
    std::size_t y = 0;
    const auto next_byte = [&state, &x, &y]() -> unsigned char {
        x = (x + 1) % 256;
        y = (y + state[x]) % 256;
        std::swap(state[x], state[y]);
        return state[(state[x] + state[y]) % 256];
    };

    // Апстрим отбрасывает 1024 байта потока после инициализации ключа. Без
    // этого расшифровка ответа даёт мусор, и никакой тест формата это не
    // покажет: ответ просто не разберётся как JSON.
    for (int skipped = 0; skipped < 1024; ++skipped) {
        next_byte();
    }

    std::string out;
    out.reserve(payload.size());
    for (const char c : payload) {
        out.push_back(static_cast<char>(static_cast<unsigned char>(c) ^ next_byte()));
    }
    return out;
}

std::string signed_nonce(std::string_view ssecurity_b64, std::string_view nonce_raw) {
    const std::string ssecurity = b64_decode(ssecurity_b64);
    std::string buffer;
    buffer.reserve(ssecurity.size() + nonce_raw.size());
    buffer.append(ssecurity);
    buffer.append(nonce_raw);

    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(buffer.data()), buffer.size(), digest.data());
    return std::string(reinterpret_cast<const char*>(digest.data()), digest.size());
}

std::string make_nonce(std::int64_t minutes_since_epoch, std::string_view random8) {
    if (random8.size() != 8) {
        throw MiFitnessAuthError("make_nonce: need exactly eight random bytes");
    }
    std::string nonce(random8);
    const auto value = static_cast<std::uint32_t>(minutes_since_epoch);
    for (int shift = 24; shift >= 0; shift -= 8) {
        nonce.push_back(static_cast<char>((value >> static_cast<unsigned int>(shift)) & 0xff));
    }
    return nonce;
}

std::string random_bytes(std::size_t count) {
    std::string out(count, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), static_cast<int>(count)) != 1) {
        throw MiFitnessAuthError("random_bytes: OpenSSL RAND_bytes failed");
    }
    return out;
}

std::string signature(std::string_view method,
                      std::string_view path,
                      std::string_view data,
                      std::optional<std::string_view> rc4_hash,
                      std::string_view signed_nonce_raw) {
    std::string base;
    base.append(method).append("&").append(path).append("&data=").append(data);
    if (rc4_hash.has_value()) {
        base.append("&rc4_hash__=").append(*rc4_hash);
    }
    base.append("&").append(b64_encode(signed_nonce_raw));

    std::array<unsigned char, SHA_DIGEST_LENGTH> digest{};
    SHA1(reinterpret_cast<const unsigned char*>(base.data()), base.size(), digest.data());
    return b64_encode(std::string(reinterpret_cast<const char*>(digest.data()), digest.size()));
}

}  // namespace Xiaomi::Crypto
