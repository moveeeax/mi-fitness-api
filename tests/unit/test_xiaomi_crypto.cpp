/**
 * @file test_xiaomi_crypto.cpp
 * @brief Порт крипты Xiaomi против золотых векторов Python-реализации.
 *
 * Векторы в tests/fixtures/xiaomi_crypto_vectors.json сняты с апстрима
 * скриптом tools/gen_crypto_vectors.py. Смысл именно в них: RC4 у Xiaomi
 * нестандартный (после инициализации ключа отбрасывается 1024 байта потока),
 * а порядок сборки подписи проверить рассуждением нельзя. Векторы ловят
 * ошибку переноса без сети и без живого аккаунта.
 */

#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "xiaomi/Crypto.hpp"

namespace {

nlohmann::json load_vectors() {
    // Путь от корня репозитория: так же читают спеку тесты e2e.
    std::ifstream in("tests/fixtures/xiaomi_crypto_vectors.json");
    if (!in) {
        throw std::runtime_error("fixture not found; run tools/gen_crypto_vectors.py from the repo root");
    }
    nlohmann::json j;
    in >> j;
    return j;
}

}  // namespace

TEST(XiaomiCrypto, Rc4MatchesUpstreamVectors) {
    const auto vectors = load_vectors()["rc4"];
    ASSERT_EQ(vectors.size(), 12u) << "фикстура не та, что ожидает тест";
    for (const auto& v : vectors) {
        const auto key = Xiaomi::Crypto::b64_decode(v["key_b64"].get<std::string>());
        const auto payload = Xiaomi::Crypto::b64_decode(v["payload_b64"].get<std::string>());
        EXPECT_EQ(Xiaomi::Crypto::b64_encode(Xiaomi::Crypto::rc4(key, payload)), v["cipher_b64"].get<std::string>());
    }
}

TEST(XiaomiCrypto, Rc4IsItsOwnInverse) {
    const std::string key = "secret-material!";
    const std::string clear = R"({"start_time":1,"end_time":2})";
    EXPECT_EQ(Xiaomi::Crypto::rc4(key, Xiaomi::Crypto::rc4(key, clear)), clear);
}

TEST(XiaomiCrypto, SignedNonceMatchesUpstreamVectors) {
    for (const auto& v : load_vectors()["signed_nonce"]) {
        const auto nonce = Xiaomi::Crypto::b64_decode(v["nonce_b64"].get<std::string>());
        EXPECT_EQ(
            Xiaomi::Crypto::b64_encode(Xiaomi::Crypto::signed_nonce(v["ssecurity_b64"].get<std::string>(), nonce)),
            v["signed_nonce_b64"].get<std::string>());
    }
}

TEST(XiaomiCrypto, SignatureMatchesUpstreamVectors) {
    for (const auto& v : load_vectors()["signature"]) {
        const auto sn = Xiaomi::Crypto::b64_decode(v["signed_nonce_b64"].get<std::string>());
        std::string hash_storage;
        std::optional<std::string_view> rc4_hash;
        if (!v["rc4_hash"].is_null()) {
            hash_storage = v["rc4_hash"].get<std::string>();
            rc4_hash = hash_storage;
        }
        EXPECT_EQ(Xiaomi::Crypto::signature(v["method"].get<std::string>(),
                                            v["path"].get<std::string>(),
                                            v["data"].get<std::string>(),
                                            rc4_hash,
                                            sn),
                  v["signature"].get<std::string>());
    }
}

// Пустой ключ в Python давал исключение при делении по модулю нуля.
// В C++ это неопределённое поведение, поэтому отказ обязан быть явным.
TEST(XiaomiCrypto, EmptyKeyIsRejected) {
    EXPECT_THROW(Xiaomi::Crypto::rc4("", "payload"), Xiaomi::MiFitnessAuthError);
}

// Nonce: 8 байт случайности плюс 4 байта big-endian с номером минуты.
TEST(XiaomiCrypto, NonceLayoutIsTwelveBytesBigEndianMinutes) {
    const std::string nonce = Xiaomi::Crypto::make_nonce(0x01020304, std::string(8, '\x00'));
    ASSERT_EQ(nonce.size(), 12u);
    EXPECT_EQ(static_cast<unsigned char>(nonce[8]), 0x01);
    EXPECT_EQ(static_cast<unsigned char>(nonce[9]), 0x02);
    EXPECT_EQ(static_cast<unsigned char>(nonce[10]), 0x03);
    EXPECT_EQ(static_cast<unsigned char>(nonce[11]), 0x04);
}

TEST(XiaomiCrypto, NonceRejectsWrongRandomLength) {
    EXPECT_THROW(Xiaomi::Crypto::make_nonce(1, std::string(7, '\x00')), Xiaomi::MiFitnessAuthError);
}

// Стандартный base64 с паддингом, не base64url из Utils::Base64.
TEST(XiaomiCrypto, Base64IsStandardWithPadding) {
    EXPECT_EQ(Xiaomi::Crypto::b64_encode(std::string("\xff\xfe", 2)), "//4=");
    EXPECT_EQ(Xiaomi::Crypto::b64_decode("//4="), std::string("\xff\xfe", 2));
    EXPECT_EQ(Xiaomi::Crypto::b64_encode(""), "");
    EXPECT_THROW(Xiaomi::Crypto::b64_decode("not base64!"), Xiaomi::MiFitnessAuthError);
}
