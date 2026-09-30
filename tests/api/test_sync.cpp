/**
 * @file test_sync.cpp
 * @brief Маршруты синка: постановка запуска в очередь и чтение журнала.
 */

#include <string>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/SyncController.hpp"
#include "database/Database.hpp"
#include "jobs/Jobs.hpp"
#include "repositories/SyncRunRepository.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

namespace {

class SyncApiTest : public TestHelpers::CoreBackedTest {
protected:
    Api::SyncController controller;

    std::string config_file_name() const override { return "sync_api_test_config.json"; }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE sync_runs");
            return true;
        });
    }

    HttpResponsePtr post_sync(const json& body) {
        HttpResponsePtr captured;
        controller.enqueue(TestHelpers::make_request(Post, body), [&](const HttpResponsePtr& r) { captured = r; });
        return captured;
    }

    HttpResponsePtr get_status(const std::string& id) {
        HttpResponsePtr captured;
        controller.status(
            TestHelpers::make_request(Get), [&](const HttpResponsePtr& r) { captured = r; }, id);
        return captured;
    }
};

}  // namespace

TEST_F(SyncApiTest, EnqueueCreatesARunAndAJob) {
    auto resp = post_sync({{"from", "2026-09-22"}, {"to", "2026-09-23"}});

    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k202Accepted);
    const auto body = json::parse(std::string(resp->body()));
    ASSERT_TRUE(body.contains("data")) << body.dump();
    const long run_id = body["data"]["run_id"].get<long>();
    EXPECT_EQ(body["data"]["status"], "queued");

    // Строка журнала существует и несёт диапазон.
    Repositories::SyncRunRepository runs;
    const auto row = runs.get(run_id);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ((*row)["status"], "queued");
    EXPECT_EQ((*row)["requested_start"], "2026-09-22");

    // Задание лежит в очереди с тем же run_id.
    auto job = Jobs::get().pick({"xiaomi_sync"}, 1);
    ASSERT_TRUE(job.has_value());
    EXPECT_EQ(job->payload["run_id"].get<long>(), run_id);
}

TEST_F(SyncApiTest, EnqueueRejectsMalformedRange) {
    EXPECT_EQ(post_sync({{"from", "22.09.2026"}, {"to", "2026-09-23"}})->statusCode(), k400BadRequest);
    EXPECT_EQ(post_sync({{"from", "2026-09-24"}, {"to", "2026-09-23"}})->statusCode(), k400BadRequest);
    EXPECT_EQ(post_sync(json::object())->statusCode(), k400BadRequest);
}

TEST_F(SyncApiTest, EnqueueRejectsUnknownDataType) {
    auto resp = post_sync({{"from", "2026-09-22"}, {"to", "2026-09-23"}, {"data_types", {"nonsense"}}});
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
}

TEST_F(SyncApiTest, StatusReturnsTheJournalEntry) {
    Repositories::SyncRunRepository runs;
    const long id = runs.create("2026-09-01", "2026-09-07", {"sleep"});
    runs.finish(id, "succeeded", json{{"sleep", {{"added", 5}}}});

    auto resp = get_status(std::to_string(id));
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    const auto body = json::parse(std::string(resp->body()));
    ASSERT_TRUE(body.contains("data")) << body.dump();
    EXPECT_EQ(body["data"]["status"], "succeeded");
    EXPECT_EQ(body["data"]["result"]["sleep"]["added"], 5);
}

TEST_F(SyncApiTest, StatusRejectsUnknownAndMalformedIds) {
    EXPECT_EQ(get_status("999999")->statusCode(), k404NotFound);
    EXPECT_EQ(get_status("not-a-number")->statusCode(), k400BadRequest);
}
