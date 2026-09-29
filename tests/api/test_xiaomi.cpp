/**
 * @file test_xiaomi.cpp
 * @brief Маршрут проверки связи с облаком: подделка транспорта вместо Xiaomi.
 */

#include <string>
#include <utility>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "FakeHttpTransport.hpp"
#include "api/Api.hpp"
#include "database/Database.hpp"
#include "repositories/CredentialsRepository.hpp"
#include "test_helpers.hpp"
#include "xiaomi/Service.hpp"

using json = nlohmann::json;
using namespace drogon;

namespace {

constexpr const char* kTestKeyB64 = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";

class XiaomiProbeTest : public TestHelpers::CoreBackedTest {
protected:
    FakeHttpTransport transport;
    Api::XiaomiController controller;

    std::string config_file_name() const override { return "xiaomi_probe_test_config.json"; }

    void config_overrides(nlohmann::json& cfg) override { cfg["xiaomi"]["token_key"] = kTestKeyB64; }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Xiaomi::Service::install_for_testing(&transport);
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE xiaomi_credentials");
            return true;
        });
    }

    void TearDown() override {
        Xiaomi::Service::install_for_testing(nullptr);
        TestHelpers::CoreBackedTest::TearDown();
    }

    void seed_credentials() {
        Repositories::CredentialsRepository(kTestKeyB64).store({"1234567890", std::string(347, 'S'), "cn"});
    }

    HttpResponsePtr probe(const std::string& key, const std::string& from, const std::string& to) {
        auto request = TestHelpers::make_request(Get);
        request->setParameter("key", key);
        request->setParameter("from", from);
        request->setParameter("to", to);
        HttpResponsePtr captured;
        controller.probe(request, [&](const HttpResponsePtr& r) { captured = r; });
        return captured;
    }
};

}  // namespace

TEST_F(XiaomiProbeTest, ReturnsMaskedAccountAndCount) {
    seed_credentials();
    transport.reply_login();
    transport.reply_encrypted(R"({"code":0,"result":{"data_list":[{"a":1},{"a":2}],"has_more":false}})");

    auto resp = probe("steps", "2026-09-22", "2026-09-22");

    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    const auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["data"]["account"], "******90");
    EXPECT_EQ(body["data"]["region"], "cn");
    EXPECT_EQ(body["data"]["key"], "steps");
    EXPECT_EQ(body["data"]["records"], 2);
    // Полный идентификатор аккаунта наружу не выходит.
    EXPECT_EQ(std::string(resp->body()).find("1234567890"), std::string::npos);
}

TEST_F(XiaomiProbeTest, RejectsUnknownKey) {
    seed_credentials();
    auto resp = probe("nonsense", "2026-09-22", "2026-09-22");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
    EXPECT_TRUE(transport.requests().empty()) << "до облака дойти не должно";
}

TEST_F(XiaomiProbeTest, RejectsReversedRange) {
    seed_credentials();
    auto resp = probe("steps", "2026-09-23", "2026-09-22");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
    EXPECT_TRUE(transport.requests().empty());
}

TEST_F(XiaomiProbeTest, RejectsMalformedDate) {
    seed_credentials();
    auto resp = probe("steps", "22.09.2026", "2026-09-22");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
}

// Учётные данные ещё не посеяны: это состояние конфигурации, а не ошибка
// клиента и не ошибка облака.
TEST_F(XiaomiProbeTest, WithoutCredentialsReportsNotConfigured) {
    auto resp = probe("steps", "2026-09-22", "2026-09-22");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k503ServiceUnavailable);
    EXPECT_EQ(json::parse(std::string(resp->body()))["error"], "not_configured");
}

// Облако отвергло сохранённый токен: код upstream_auth отличим от прочих
// отказов, потому что лечится только свежим токеном, а не повтором.
TEST_F(XiaomiProbeTest, UpstreamAuthFailureIsReported) {
    seed_credentials();
    transport.reply({200, "no start prefix at all", {}});

    auto resp = probe("steps", "2026-09-22", "2026-09-22");

    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k503ServiceUnavailable);
    EXPECT_EQ(json::parse(std::string(resp->body()))["error"], "upstream_auth");
}
