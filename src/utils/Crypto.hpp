/**
 * @file Crypto.hpp
 * @brief HMAC-SHA256 + constant-time compare + random hex.
 *
 * Centralized so Auth (JWT), Tokens (link tokens) and any future primitive
 * share one implementation instead of carrying near-identical copies.
 *
 * Declarations only — the bodies live in Crypto.cpp (compiled once into
 * app_core; ADR 0003 as amended 2026-08-22): including this header no longer
 * pulls the OpenSSL headers into the including TU. Only std types survive in
 * the signatures.
 */

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace Utils::Crypto {

namespace detail {
/// Lowercase hex encoding of @p n bytes. Single source for random_hex /
/// sha256_hex — and for the external hex-of-digest callers (Storage's SigV4
/// signing, Webhooks' HMAC signature header), which is why it is declared
/// here rather than being file-local to Crypto.cpp.
std::string bytes_to_hex(const unsigned char* data, std::size_t n);
}  // namespace detail

/// Raw (binary) HMAC-SHA256 of @p data under @p key. Throws on OpenSSL failure.
std::string hmac_sha256(std::string_view key, std::string_view data);

/// Length-guarded CRYPTO_memcmp wrapper: timing-safe equality for MACs/tokens.
bool constant_time_equals(std::string_view a, std::string_view b);

/**
 * @brief Random hex string of @p byte_count bytes (so output is byte_count*2 chars).
 *        Fails closed: throws if the CSPRNG is unavailable rather than degrading
 *        to a guessable clock value (this feeds refresh-token JTIs / link tokens,
 *        where a predictable value would weaken the revocation namespace).
 */
std::string random_hex(std::size_t byte_count);

/**
 * @brief Lowercase hex SHA-256 of @p s. Used by the Idempotency middleware
 *        to fingerprint request/response bodies.
 */
std::string sha256_hex(std::string_view s);

}  // namespace Utils::Crypto
