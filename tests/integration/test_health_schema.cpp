/**
 * @file test_health_schema.cpp
 * @brief Схема данных здоровья: таблицы на месте, ключи уникальности работают,
 *        журнал запусков пишет и читает.
 */

#include <string>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "database/Database.hpp"
#include "repositories/SyncRunRepository.hpp"
#include "test_helpers.hpp"

namespace {

class HealthSchemaTest : public TestHelpers::CoreBackedTest {
protected:
    std::string config_file_name() const override { return "health_schema_test_config.json"; }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE daily_activity, sync_runs");
            return true;
        });
    }

    static long count_rows(const std::string& table) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec("SELECT COUNT(*) FROM " + table);
            return r[0][0].template as<long>();
        });
    }

    static void insert_activity(const std::string& date, int steps) {
        Database::get().execute_write([&](auto& txn) {
            txn.exec_params(
                "INSERT INTO daily_activity (user_id, date, steps) VALUES ($1, $2, $3)", "1234567890", date, steps);
            return true;
        });
    }
};

}  // namespace

TEST_F(HealthSchemaTest, AllTablesExistWithExpectedNames) {
    for (const char* table : {"daily_activity",
                              "sleep_sessions",
                              "workouts",
                              "body_measurements",
                              "heart_rate_samples",
                              "spo2_samples",
                              "stress_samples",
                              "abnormal_heart_beat_events",
                              "sync_state",
                              "sync_runs"}) {
        EXPECT_NO_THROW(count_rows(table)) << table;
    }
}

// device_id по умолчанию пустая строка и входит в ключ: NULL в уникальном
// индексе Postgres не сравнивается, и дубликаты бы молча проходили.
TEST_F(HealthSchemaTest, DuplicateNaturalKeyIsRejectedWithoutOnConflict) {
    insert_activity("2026-09-22", 100);
    EXPECT_THROW(insert_activity("2026-09-22", 200), std::exception);
    EXPECT_EQ(count_rows("daily_activity"), 1);
}

TEST_F(HealthSchemaTest, SyncRunLifecycle) {
    Repositories::SyncRunRepository runs;
    const long id = runs.create("2026-09-01", "2026-09-07", {"steps", "sleep"});
    ASSERT_GT(id, 0);

    runs.finish(id, "succeeded", nlohmann::json{{"steps", {{"added", 3}, {"updated", 1}}}});

    const auto row = runs.get(id);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ((*row)["status"], "succeeded");
    EXPECT_EQ((*row)["result"]["steps"]["added"], 3);
    EXPECT_FALSE((*row)["finished_at"].is_null());
    EXPECT_EQ((*row)["data_types"].size(), 2u);
}

TEST_F(HealthSchemaTest, UnknownRunIsEmpty) {
    Repositories::SyncRunRepository runs;
    EXPECT_FALSE(runs.get(999999).has_value());
}
