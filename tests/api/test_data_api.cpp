/**
 * @file test_data_api.cpp
 * @brief Маршруты чтения данных здоровья: диапазон, фильтры, пагинация,
 *        сводка по дням. База настоящая, строки сеются репозиториями синка.
 */

#include <string>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/DataController.hpp"
#include "database/Database.hpp"
#include "repositories/ActivityRepository.hpp"
#include "repositories/SamplesRepository.hpp"
#include "repositories/SleepRepository.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

namespace {

class DataApiTest : public TestHelpers::CoreBackedTest {
protected:
    Api::DataController controller;

    std::string config_file_name() const override { return "data_api_test_config.json"; }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Database::get().execute_write([](auto& txn) {
            txn.exec(
                "TRUNCATE TABLE daily_activity, sleep_sessions, heart_rate_samples, "
                "stress_samples, spo2_samples, body_measurements, workouts");
            return true;
        });
    }

    static HttpRequestPtr ranged(const std::string& from, const std::string& to) {
        auto req = TestHelpers::make_request(Get);
        req->setParameter("from", from);
        req->setParameter("to", to);
        return req;
    }

    static json body_of(const HttpResponsePtr& resp) { return json::parse(std::string(resp->body())); }

    void seed_activity(const std::string& date, long steps) {
        Domain::DailyActivity d;
        d.user_id = "42";
        d.date = date;
        d.steps = steps;
        d.distance_m = 100.0 * static_cast<double>(steps) / 100.0;
        Repositories::ActivityRepository().upsert({d});
    }

    void seed_heart_rate(const std::string& ts, int bpm, const std::string& type) {
        Domain::HeartRateSample s;
        s.user_id = "42";
        s.timestamp = ts;
        s.bpm = bpm;
        s.sample_type = type;
        Repositories::SamplesRepository().upsert_heart_rate({s});
    }
};

}  // namespace

TEST_F(DataApiTest, DailyActivityReturnsRowsInRange) {
    seed_activity("2026-09-20", 1000);
    seed_activity("2026-09-21", 2000);
    seed_activity("2026-09-25", 3000);

    HttpResponsePtr resp;
    controller.dailyActivity(ranged("2026-09-20", "2026-09-22"), [&](const HttpResponsePtr& r) { resp = r; });

    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK) << resp->body();
    const auto body = body_of(resp);
    EXPECT_EQ(body["count"], 2);
    EXPECT_EQ(body["total"], 2);
    ASSERT_EQ(body["data"].size(), 2u);
    EXPECT_EQ(body["data"][0]["date"], "2026-09-20");
    EXPECT_EQ(body["data"][0]["steps"], 1000);
    EXPECT_EQ(body["data"][1]["date"], "2026-09-21");
}

TEST_F(DataApiTest, RangeIsValidated) {
    HttpResponsePtr resp;
    controller.dailyActivity(ranged("20.09.2026", "2026-09-22"), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);

    auto no_to = TestHelpers::make_request(Get);
    no_to->setParameter("from", "2026-09-20");
    HttpResponsePtr resp2;
    controller.dailyActivity(no_to, [&](const HttpResponsePtr& r) { resp2 = r; });
    ASSERT_NE(resp2, nullptr);
    EXPECT_EQ(resp2->statusCode(), k400BadRequest);
}

TEST_F(DataApiTest, HeartRateFiltersByTypeAndPaginates) {
    seed_heart_rate("2026-09-21T10:00:00+00:00", 70, "passive");
    seed_heart_rate("2026-09-21T11:00:00+00:00", 80, "passive");
    seed_heart_rate("2026-09-21T00:00:00+00:00", 55, "resting");

    HttpResponsePtr by_type;
    auto req = ranged("2026-09-21", "2026-09-21");
    req->setParameter("type", "resting");
    controller.heartRate(req, [&](const HttpResponsePtr& r) { by_type = r; });
    ASSERT_NE(by_type, nullptr);
    ASSERT_EQ(by_type->statusCode(), k200OK) << by_type->body();
    auto body = body_of(by_type);
    ASSERT_EQ(body["data"].size(), 1u);
    EXPECT_EQ(body["data"][0]["bpm"], 55);

    HttpResponsePtr page;
    auto preq = ranged("2026-09-21", "2026-09-21");
    preq->setParameter("limit", "1");
    preq->setParameter("offset", "1");
    controller.heartRate(preq, [&](const HttpResponsePtr& r) { page = r; });
    ASSERT_NE(page, nullptr);
    body = body_of(page);
    EXPECT_EQ(body["total"], 3);
    EXPECT_EQ(body["count"], 1);
    ASSERT_EQ(body["data"].size(), 1u);
    // Сортировка по времени: вторая строка это 10:00.
    EXPECT_EQ(body["data"][0]["bpm"], 70);
}

TEST_F(DataApiTest, SleepRowsCarrySessionFields) {
    Domain::SleepSession s;
    s.user_id = "42";
    s.sleep_id = "sess-1";
    s.start_at = "2026-09-20T23:00:00+00:00";
    s.end_at = "2026-09-21T06:30:00+00:00";
    s.start_epoch = 1789858800;
    s.end_epoch = 1789885800;
    s.duration_minutes = 450;
    s.time_asleep_minutes = 430;
    s.time_awake_minutes = 20;
    s.sleep_score = 77;
    s.sleep_score_source = "daily_report";
    Repositories::SleepRepository().upsert({s});

    HttpResponsePtr resp;
    controller.sleep(ranged("2026-09-21", "2026-09-21"), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK) << resp->body();
    const auto body = body_of(resp);
    ASSERT_EQ(body["data"].size(), 1u);
    const auto& row = body["data"][0];
    EXPECT_EQ(row["sleep_id"], "sess-1");
    EXPECT_EQ(row["duration_minutes"], 450);
    EXPECT_EQ(row["sleep_score"], 77);
    EXPECT_EQ(row["sleep_score_source"], "daily_report");
    EXPECT_EQ(row["is_nap"], false);
}

TEST_F(DataApiTest, SummaryJoinsTheDay) {
    seed_activity("2026-09-21", 5000);
    seed_heart_rate("2026-09-21T00:00:00+00:00", 55, "resting");
    Domain::SleepSession s;
    s.user_id = "42";
    s.sleep_id = "sess-2";
    s.start_at = "2026-09-20T23:00:00+00:00";
    s.end_at = "2026-09-21T06:30:00+00:00";
    s.start_epoch = 1789858800;
    s.end_epoch = 1789885800;
    s.duration_minutes = 450;
    s.time_asleep_minutes = 430;
    s.time_awake_minutes = 20;
    s.sleep_score = 77;
    Repositories::SleepRepository().upsert({s});

    HttpResponsePtr resp;
    controller.summary(ranged("2026-09-21", "2026-09-21"), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK) << resp->body();
    const auto body = body_of(resp);
    ASSERT_EQ(body["data"].size(), 1u);
    const auto& row = body["data"][0];
    EXPECT_EQ(row["date"], "2026-09-21");
    EXPECT_EQ(row["steps"], 5000);
    EXPECT_EQ(row["sleep_duration_minutes"], 450);
    EXPECT_EQ(row["sleep_score"], 77);
    EXPECT_EQ(row["resting_bpm"], 55);
}
