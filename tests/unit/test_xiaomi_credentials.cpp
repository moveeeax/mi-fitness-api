/**
 * @file test_xiaomi_credentials.cpp
 * @brief Валидация токена, маскирование идентификатора, secretbox. Без базы.
 */

#include <string>

#include <gtest/gtest.h>

#include "xiaomi/Credentials.hpp"

namespace {
// 32 нулевых байта в base64: ключ шифрования для тестов.
constexpr const char* kTestKeyB64 = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
constexpr const char* kOtherKeyB64 = "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=";
}  // namespace

// Ровно тот случай, который однажды стоил часа разбирательств: DevTools
// показывает длинную куку с многоточием в середине, и скопированное значение
// приезжает с U+2026 внутри. Заголовок Cookie обязан быть ASCII, поэтому
// падало это уже на сетевом запросе непонятной ошибкой кодирования.
TEST(XiaomiCredentials, RejectsNonAsciiToken) {
    const std::string token = std::string("AXSU") + "\xe2\x80\xa6" + "CqGAl";
    try {
        Xiaomi::validate_pass_token(token);
        FAIL() << "ожидался отказ на символе вне ASCII";
    } catch (const Xiaomi::MiFitnessAuthError& e) {
        // Позиция нужна, чтобы владелец понял, где обрезалось значение.
        EXPECT_NE(std::string(e.what()).find("position 4"), std::string::npos) << "сообщение: " << e.what();
        // Само значение в текст ошибки попадать не должно: он уедет в логи.
        EXPECT_EQ(std::string(e.what()).find("AXSU"), std::string::npos);
    }
}

TEST(XiaomiCredentials, RejectsSemicolonWhitespaceAndEmpty) {
    EXPECT_THROW(Xiaomi::validate_pass_token("abc;def"), Xiaomi::MiFitnessAuthError);
    EXPECT_THROW(Xiaomi::validate_pass_token("abc def"), Xiaomi::MiFitnessAuthError);
    EXPECT_THROW(Xiaomi::validate_pass_token("abc\tdef"), Xiaomi::MiFitnessAuthError);
    EXPECT_THROW(Xiaomi::validate_pass_token("abc\ndef"), Xiaomi::MiFitnessAuthError);
    EXPECT_THROW(Xiaomi::validate_pass_token(""), Xiaomi::MiFitnessAuthError);
}

TEST(XiaomiCredentials, AcceptsRealisticToken) {
    // Живой токен это 347 печатных символов ASCII.
    EXPECT_NO_THROW(Xiaomi::validate_pass_token(std::string(347, 'A')));
}

TEST(XiaomiCredentials, MaskKeepsOnlyLastTwoCharacters) {
    EXPECT_EQ(Xiaomi::mask_account_id("1234567890"), "******90");
    EXPECT_EQ(Xiaomi::mask_account_id("42"), "******");
    EXPECT_EQ(Xiaomi::mask_account_id("4"), "******");
    EXPECT_EQ(Xiaomi::mask_account_id(""), "");
    // Длина исходного значения по маске не восстанавливается: звёзд всегда шесть.
    EXPECT_EQ(Xiaomi::mask_account_id("123456789012345").size(), 8u);
}

TEST(XiaomiCredentials, SealRoundTrips) {
    const std::string plain(347, 'T');
    const Xiaomi::Sealed sealed = Xiaomi::seal(plain, kTestKeyB64);
    EXPECT_NE(sealed.ciphertext, plain);
    EXPECT_EQ(sealed.nonce.size(), 24u);
    // Шифротекст длиннее открытого текста на тег аутентичности.
    EXPECT_EQ(sealed.ciphertext.size(), plain.size() + 16u);
    EXPECT_EQ(Xiaomi::unseal(sealed, kTestKeyB64), plain);
}

TEST(XiaomiCredentials, SealUsesAFreshNonceEveryTime) {
    const Xiaomi::Sealed first = Xiaomi::seal("token", kTestKeyB64);
    const Xiaomi::Sealed second = Xiaomi::seal("token", kTestKeyB64);
    EXPECT_NE(first.nonce, second.nonce);
    EXPECT_NE(first.ciphertext, second.ciphertext);
}

TEST(XiaomiCredentials, UnsealWithWrongKeyFails) {
    const Xiaomi::Sealed sealed = Xiaomi::seal("secret", kTestKeyB64);
    EXPECT_THROW(Xiaomi::unseal(sealed, kOtherKeyB64), Xiaomi::MiFitnessAuthError);
}

TEST(XiaomiCredentials, UnsealDetectsTamperedCiphertext) {
    Xiaomi::Sealed sealed = Xiaomi::seal("secret", kTestKeyB64);
    sealed.ciphertext[0] = static_cast<char>(sealed.ciphertext[0] ^ 0x01);
    EXPECT_THROW(Xiaomi::unseal(sealed, kTestKeyB64), Xiaomi::MiFitnessAuthError);
}

TEST(XiaomiCredentials, RejectsKeyOfWrongLength) {
    EXPECT_THROW(Xiaomi::seal("secret", "QUJD"), Xiaomi::MiFitnessAuthError);
    EXPECT_THROW(Xiaomi::seal("secret", ""), Xiaomi::MiFitnessAuthError);
}
