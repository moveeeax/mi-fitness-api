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

TEST_F(DataApiTest, CoverageReportsPerTypeBounds) {
    seed_activity("2026-09-20", 1000);
    seed_activity("2026-09-25", 3000);
    seed_heart_rate("2026-09-21T10:00:00+00:00", 70, "passive");

    HttpResponsePtr resp;
    controller.coverage(TestHelpers::make_request(Get), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK) << resp->body();
    const auto body = body_of(resp);
    const auto& act = body["data"]["daily_activity"];
    EXPECT_EQ(act["first_date"], "2026-09-20");
    EXPECT_EQ(act["last_date"], "2026-09-25");
    EXPECT_EQ(act["records"], 2);
    const auto& hr = body["data"]["heart_rate"];
    EXPECT_EQ(hr["records"], 1);
    // Пустой тип это NULL-границы, не мусор.
    EXPECT_TRUE(body["data"]["workouts"]["first_date"].is_null());
    EXPECT_EQ(body["data"]["workouts"]["records"], 0);
}

TEST_F(DataApiTest, ExportJsonCarriesTheBridgeEnvelope) {
    seed_activity("2026-09-21", 5000);
    seed_heart_rate("2026-09-21T10:00:00+00:00", 70, "passive");

    auto req = ranged("2026-09-21", "2026-09-21");
    req->setParameter("format", "json");
    HttpResponsePtr resp;
    controller.exportData(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK) << resp->body();
    const auto body = body_of(resp);
    EXPECT_EQ(body["schema_version"], "1.0");
    EXPECT_EQ(body["source"], "mi-fitness-api");
    EXPECT_EQ(body["filters"]["start_date"], "2026-09-21");
    ASSERT_TRUE(body["records"].contains("daily_activity"));
    EXPECT_EQ(body["records"]["daily_activity"].size(), 1u);
    EXPECT_EQ(body["records"]["heart_rate"].size(), 1u);
}

TEST_F(DataApiTest, ExportCsvNeedsATypeAndEscapesFormulas) {
    Domain::DailyActivity d;
    d.user_id = "42";
    d.date = "2026-09-21";
    d.steps = 5000;
    d.timezone = "=SUM(A1:A9)";  // формула в строковом поле не должна выжить
    Repositories::ActivityRepository().upsert({d});

    auto no_type = ranged("2026-09-21", "2026-09-21");
    no_type->setParameter("format", "csv");
    HttpResponsePtr bad;
    controller.exportData(no_type, [&](const HttpResponsePtr& r) { bad = r; });
    ASSERT_NE(bad, nullptr);
    EXPECT_EQ(bad->statusCode(), k400BadRequest);

    auto req = ranged("2026-09-21", "2026-09-21");
    req->setParameter("format", "csv");
    req->setParameter("type", "daily_activity");
    HttpResponsePtr resp;
    controller.exportData(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK) << resp->body();
    const std::string body(resp->body());
    EXPECT_NE(body.find("date"), std::string::npos);
    EXPECT_NE(body.find("5000"), std::string::npos);
    // Ведущий знак равенства экранирован апострофом.
    EXPECT_EQ(body.find(",=SUM"), std::string::npos);
    EXPECT_NE(body.find("'=SUM"), std::string::npos);
}

// Обзор фазы 3, Important 3: события аномального пульса синкаются, но не
// читались и не выгружались, а мост, который их выгружал, удалён.
TEST_F(DataApiTest, AbnormalHeartBeatIsReadableAndExported) {
    Database::get().execute_write([](auto& txn) {
        txn.exec(
            "INSERT INTO abnormal_heart_beat_events (user_id, event_id, start_at, end_at, duration_seconds) "
            "VALUES ('42', 'ev-1', '2026-09-21T10:00:00+00:00', '2026-09-21T10:01:00+00:00', 60)");
        return true;
    });

    HttpResponsePtr resp;
    controller.abnormalHeartBeat(ranged("2026-09-21", "2026-09-21"), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK) << resp->body();
    auto body = body_of(resp);
    ASSERT_EQ(body["data"].size(), 1u);
    EXPECT_EQ(body["data"][0]["event_id"], "ev-1");

    auto req = ranged("2026-09-21", "2026-09-21");
    req->setParameter("format", "json");
    HttpResponsePtr exp;
    controller.exportData(req, [&](const HttpResponsePtr& r) { exp = r; });
    ASSERT_NE(exp, nullptr);
    const auto envelope = body_of(exp);
    ASSERT_TRUE(envelope["records"].contains("abnormal_heart_beat")) << envelope.dump();
    EXPECT_EQ(envelope["records"]["abnormal_heart_beat"].size(), 1u);
}

// Обзор фазы 3, Important 5: date у активности локальная (пояс устройства),
// а окна сводки строились от суток UTC — утренний resting-пульс уезжал в
// предыдущую строку. Окно считается в поясе региона (+08 для cn).
TEST_F(DataApiTest, SummaryWindowFollowsTheRegionZone) {
    seed_activity("2026-09-21", 5000);
    // 05:00 +08:00 двадцать первого = 21:00Z двадцатого: по UTC-окну это
    // предыдущие сутки.
    seed_heart_rate("2026-09-20T21:00:00+00:00", 55, "resting");

    HttpResponsePtr resp;
    controller.summary(ranged("2026-09-21", "2026-09-21"), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK) << resp->body();
    const auto body = body_of(resp);
    ASSERT_EQ(body["data"].size(), 1u);
    EXPECT_EQ(body["data"][0]["resting_bpm"], 55);
}

// Обзор фазы 3, Important 8: экспорт без потолка ширины окна собирал в
// память годы данных одним значением json_agg.
TEST_F(DataApiTest, ExportRejectsARangeWiderThanAYear) {
    auto req = ranged("2020-01-01", "2030-01-01");
    req->setParameter("format", "json");
    HttpResponsePtr resp;
    controller.exportData(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
    EXPECT_EQ(body_of(resp)["error"], "range_too_wide");
}
