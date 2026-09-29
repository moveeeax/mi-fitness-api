/**
 * @file test_xiaomi_seed.cpp
 * @brief Посев учётных данных Xiaomi из окружения при старте сервиса.
 *
 * Secret кластера это только сид: если в базе уже лежит ротированный токен,
 * значения из окружения его не перетирают. Иначе каждый перезапуск пода
 * откатывал бы токен к устаревшему, и первый же логин падал бы.
 */

#include <string>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "database/Database.hpp"
#include "repositories/CredentialsRepository.hpp"
#include "test_helpers.hpp"
#include "xiaomi/Service.hpp"

namespace {

constexpr const char* kTestKeyB64 = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";

class XiaomiSeedTest : public TestHelpers::CoreBackedTest {
protected:
    std::string config_file_name() const override { return "xiaomi_seed_test_config.json"; }

    void config_overrides(nlohmann::json& cfg) override {
        cfg["xiaomi"]["token_key"] = kTestKeyB64;
        cfg["xiaomi"]["user_id"] = "1234567890";
        cfg["xiaomi"]["pass_token"] = std::string(347, 'S');
        cfg["xiaomi"]["region"] = "cn";
    }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE xiaomi_credentials");
            return true;
        });
    }

    Repositories::CredentialsRepository repo() { return Repositories::CredentialsRepository(kTestKeyB64); }
};

}  // namespace

TEST_F(XiaomiSeedTest, SeedsEmptyTableFromConfig) {
    EXPECT_TRUE(Xiaomi::Service::seed_credentials_if_missing());

    const auto loaded = repo().load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->user_id, "1234567890");
    EXPECT_EQ(loaded->pass_token, std::string(347, 'S'));
    EXPECT_EQ(loaded->region, "cn");
}

// Ротированный токен в базе главнее сида: Xiaomi выдал его позже, чем Secret
// был создан, и перетирание откатило бы сессию к мёртвому значению.
TEST_F(XiaomiSeedTest, DoesNotOverwriteARotatedToken) {
    repo().store({"1234567890", std::string(347, 'R'), "cn"});

    EXPECT_FALSE(Xiaomi::Service::seed_credentials_if_missing());
    EXPECT_EQ(repo().load()->pass_token, std::string(347, 'R'));
}

TEST_F(XiaomiSeedTest, SeedIsIdempotent) {
    EXPECT_TRUE(Xiaomi::Service::seed_credentials_if_missing());
    EXPECT_FALSE(Xiaomi::Service::seed_credentials_if_missing());
    EXPECT_TRUE(repo().load().has_value());
}

namespace {

// Отдельная фикстура: reseed=true это аварийный рычаг. Токен в базе умер
// (облако его отвергло), владелец кладёт свежий в Secret и включает флаг.
class XiaomiReseedTest : public XiaomiSeedTest {
protected:
    std::string config_file_name() const override { return "xiaomi_reseed_test_config.json"; }

    void config_overrides(nlohmann::json& cfg) override {
        XiaomiSeedTest::config_overrides(cfg);
        cfg["xiaomi"]["reseed"] = true;
    }
};

}  // namespace

TEST_F(XiaomiReseedTest, ReseedOverwritesTheStoredToken) {
    repo().store({"1234567890", std::string(347, 'R'), "cn"});

    EXPECT_TRUE(Xiaomi::Service::seed_credentials_if_missing());
    EXPECT_EQ(repo().load()->pass_token, std::string(347, 'S'));
}
