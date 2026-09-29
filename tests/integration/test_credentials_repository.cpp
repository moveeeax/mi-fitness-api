/**
 * @file test_credentials_repository.cpp
 * @brief Хранение ротируемого токена Xiaomi: запись, перезапись, шифрование.
 */

#include <string>

#include <gtest/gtest.h>

#include "database/Database.hpp"
#include "repositories/CredentialsRepository.hpp"
#include "test_helpers.hpp"

namespace {

// 32 нулевых байта в base64 и второй, заведомо другой ключ.
constexpr const char* kTestKeyB64 = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
constexpr const char* kOtherKeyB64 = "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=";

class CredentialsRepositoryTest : public TestHelpers::CoreBackedTest {
protected:
    std::string config_file_name() const override { return "credentials_repo_test_config.json"; }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE xiaomi_credentials");
            return true;
        });
    }

    static long count_rows() {
        return Database::get().execute_read([](auto& txn) {
            auto r = txn.exec("SELECT COUNT(*) FROM xiaomi_credentials");
            return r[0][0].template as<long>();
        });
    }

    static std::string stored_ciphertext() {
        return Database::get().execute_read([](auto& txn) {
            auto r = txn.exec("SELECT pass_token_sealed FROM xiaomi_credentials LIMIT 1");
            return r[0][0].template as<std::string>();
        });
    }
};

}  // namespace

TEST_F(CredentialsRepositoryTest, EmptyTableMeansNoCredentialsYet) {
    Repositories::CredentialsRepository repo(kTestKeyB64);
    EXPECT_FALSE(repo.load().has_value());
}

TEST_F(CredentialsRepositoryTest, RoundTripAndRotateKeepOneRow) {
    Repositories::CredentialsRepository repo(kTestKeyB64);
    repo.store({"1234567890", std::string(347, 'S'), "cn"});

    auto loaded = repo.load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->user_id, "1234567890");
    EXPECT_EQ(loaded->pass_token, std::string(347, 'S'));
    EXPECT_EQ(loaded->region, "cn");

    // Ротация обновляет ту же строку, а не копит историю мёртвых токенов.
    repo.store({"1234567890", std::string(347, 'R'), "cn"});
    EXPECT_EQ(repo.load()->pass_token, std::string(347, 'R'));
    EXPECT_EQ(count_rows(), 1);
}

TEST_F(CredentialsRepositoryTest, TokenIsNotStoredInClear) {
    Repositories::CredentialsRepository repo(kTestKeyB64);
    repo.store({"1234567890", std::string(347, 'R'), "cn"});

    const std::string stored = stored_ciphertext();
    EXPECT_EQ(stored.find(std::string(20, 'R')), std::string::npos);
    // base64 от шифротекста длиннее открытого текста: плюс тег аутентичности.
    EXPECT_GT(stored.size(), 347u);
}

TEST_F(CredentialsRepositoryTest, WrongKeyFailsLoudlyOnLoad) {
    Repositories::CredentialsRepository writer(kTestKeyB64);
    writer.store({"1234567890", std::string(347, 'S'), "cn"});

    Repositories::CredentialsRepository reader(kOtherKeyB64);
    EXPECT_THROW(reader.load(), Xiaomi::MiFitnessAuthError);
}

// Находка обзора 2: user_id уходит в заголовок Cookie рядом с токеном, а
// region подставляется в хост облака. CRLF в первом это инъекция заголовка,
// произвольная строка во втором уводит куки запроса на чужой хост.
TEST_F(CredentialsRepositoryTest, StoreRejectsUserIdWithHeaderInjection) {
    Repositories::CredentialsRepository repo(kTestKeyB64);
    EXPECT_THROW(repo.store({"123\r\nHost: evil", std::string(347, 'S'), "cn"}), Xiaomi::MiFitnessAuthError);
    EXPECT_EQ(count_rows(), 0);
}

TEST_F(CredentialsRepositoryTest, StoreRejectsUnknownRegion) {
    Repositories::CredentialsRepository repo(kTestKeyB64);
    EXPECT_THROW(repo.store({"1234567890", std::string(347, 'S'), "evil.com/x"}), Xiaomi::MiFitnessAuthError);
    EXPECT_EQ(count_rows(), 0);
}

// Мусор в базе стоит дороже отказа на входе: проверка стоит до записи.
TEST_F(CredentialsRepositoryTest, StoreRejectsTokenThatCannotGoIntoACookie) {
    Repositories::CredentialsRepository repo(kTestKeyB64);
    const std::string broken = std::string("AXSU") + "\xe2\x80\xa6" + "CqGAl";
    EXPECT_THROW(repo.store({"1234567890", broken, "cn"}), Xiaomi::MiFitnessAuthError);
    EXPECT_EQ(count_rows(), 0);
}
