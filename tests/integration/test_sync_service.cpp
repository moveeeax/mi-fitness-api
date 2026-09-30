/**
 * @file test_sync_service.cpp
 * @brief Синк целиком: облако это подделка, база настоящая.
 *
 * Review Focus плана: идемпотентность повторного прохода, изоляция провала
 * типа, класс auth-ошибки, взаимное исключение запусков, обновление sync_state.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "FakeHttpTransport.hpp"
#include "database/Database.hpp"
#include "repositories/SyncRunRepository.hpp"
#include "sync/SyncService.hpp"
#include "test_helpers.hpp"

namespace {

using nlohmann::json;

constexpr long long kNoon = 1790049600;  // 2026-09-22T12:00:00+08:00

class SyncServiceTest : public TestHelpers::CoreBackedTest {
protected:
    FakeHttpTransport transport;

    std::string config_file_name() const override { return "sync_service_test_config.json"; }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Database::get().execute_write([](auto& txn) {
            txn.exec(
                "TRUNCATE TABLE daily_activity, sleep_sessions, workouts, body_measurements, "
                "heart_rate_samples, spo2_samples, stress_samples, abnormal_heart_beat_events, "
                "sync_state, sync_runs");
            return true;
        });
    }

    static std::string steps_page(int steps) {
        const json record{{"time", kNoon},
                          {"zone_offset", 28800},
                          {"zone_name", "Asia/Shanghai"},
                          {"value", json{{"steps", steps}, {"distance", 10.0}, {"calories", 1.0}}.dump()}};
        return json{{"code", 0}, {"result", {{"data_list", json::array({record})}, {"has_more", false}}}}.dump();
    }

    static std::string weight_page() {
        const json record{{"time", kNoon}, {"zone_offset", 28800}, {"value", json{{"weight", 91.9}}.dump()}};
        return json{{"code", 0}, {"result", {{"data_list", json::array({record})}, {"has_more", false}}}}.dump();
    }

    static std::string empty_page() {
        return json{{"code", 0}, {"result", {{"data_list", json::array()}, {"has_more", false}}}}.dump();
    }

    Sync::SyncService service() { return Sync::SyncService(transport, {"1234567890", std::string(347, 'S'), "cn"}); }
};

}  // namespace

TEST_F(SyncServiceTest, RepeatRunGivesAddedThenUpdated) {
    Repositories::SyncRunRepository runs;

    transport.reply_login();
    transport.reply_encrypted(steps_page(100));  // steps
    transport.reply_encrypted(empty_page());     // calories
    const long first = runs.create("2026-09-22", "2026-09-22", {"daily_activity"});
    service().run(first, "2026-09-22", "2026-09-22", {"daily_activity"});
    auto row = runs.get(first);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ((*row)["status"], "succeeded");
    EXPECT_EQ((*row)["result"]["daily_activity"]["added"], 1);

    transport.reply_login();
    transport.reply_encrypted(steps_page(100));
    transport.reply_encrypted(empty_page());
    const long second = runs.create("2026-09-22", "2026-09-22", {"daily_activity"});
    service().run(second, "2026-09-22", "2026-09-22", {"daily_activity"});
    row = runs.get(second);
    EXPECT_EQ((*row)["result"]["daily_activity"]["added"], 0);
    EXPECT_EQ((*row)["result"]["daily_activity"]["updated"], 1);
}

// Провал одного типа не отменяет остальные, а его класс ошибки виден в журнале.
TEST_F(SyncServiceTest, FailedTypeDoesNotStopTheRest) {
    Repositories::SyncRunRepository runs;
    transport.reply_login();
    transport.reply({500, "boom", {}});        // sleep: ошибка протокола
    transport.reply_encrypted(weight_page());  // body_measurements работает

    const long id = runs.create("2026-09-22", "2026-09-22", {"sleep", "body_measurements"});
    service().run(id, "2026-09-22", "2026-09-22", {"sleep", "body_measurements"});

    const auto row = runs.get(id);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ((*row)["status"], "failed");
    EXPECT_EQ((*row)["result"]["sleep"]["error"], "protocol");
    EXPECT_EQ((*row)["result"]["body_measurements"]["added"], 1);
}

// Отказ авторизации помечается отдельно: он лечится свежим токеном, и очередь
// не должна его ретраить.
TEST_F(SyncServiceTest, AuthCodeIsClassifiedAsAuth) {
    Repositories::SyncRunRepository runs;
    transport.reply_login();
    transport.reply_encrypted(R"({"code":-10001,"message":"expired"})");
    transport.reply_encrypted(weight_page());

    const long id = runs.create("2026-09-22", "2026-09-22", {"daily_activity", "body_measurements"});
    service().run(id, "2026-09-22", "2026-09-22", {"daily_activity", "body_measurements"});

    const auto row = runs.get(id);
    EXPECT_EQ((*row)["result"]["daily_activity"]["error"], "auth");
    EXPECT_EQ((*row)["result"]["body_measurements"]["added"], 1);
}

// Вторая строка running невозможна: переход queued -> running атомарен через
// частичный уникальный индекс, конкурент честно завершается skipped.
TEST_F(SyncServiceTest, ConcurrentRunIsSkipped) {
    Repositories::SyncRunRepository runs;
    Database::get().execute_write([](auto& txn) {
        txn.exec(
            "INSERT INTO sync_runs (status, requested_start, requested_end) "
            "VALUES ('running', '2026-09-01', '2026-09-07')");
        return true;
    });

    const long id = runs.create("2026-09-22", "2026-09-22", {"daily_activity"});
    service().run(id, "2026-09-22", "2026-09-22", {"daily_activity"});

    const auto row = runs.get(id);
    EXPECT_EQ((*row)["status"], "skipped");
    EXPECT_TRUE(transport.requests().empty()) << "до облака дойти не должно";
}

TEST_F(SyncServiceTest, SyncStateIsUpdatedPerType) {
    Repositories::SyncRunRepository runs;
    transport.reply_login();
    transport.reply_encrypted(steps_page(100));
    transport.reply_encrypted(empty_page());

    const long id = runs.create("2026-09-22", "2026-09-22", {"daily_activity"});
    service().run(id, "2026-09-22", "2026-09-22", {"daily_activity"});

    const bool has_state = Database::get().execute_read([](auto& txn) {
        auto r = txn.exec(
            "SELECT last_sync_at IS NOT NULL AND records_count = 1 FROM sync_state "
            "WHERE data_type = 'daily_activity'");
        return !r.empty() && r[0][0].template as<bool>();
    });
    EXPECT_TRUE(has_state);
}
