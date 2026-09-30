/**
 * @file test_sleep_repository.cpp
 * @brief Upsert сна: идемпотентность, COALESCE оценки, стадии в jsonb.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "database/Database.hpp"
#include "repositories/SleepRepository.hpp"
#include "test_helpers.hpp"

namespace {

class SleepRepositoryTest : public TestHelpers::CoreBackedTest {
protected:
    std::string config_file_name() const override { return "sleep_repo_test_config.json"; }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE sleep_sessions");
            return true;
        });
    }

    static Domain::SleepSession session(const std::string& sleep_id, std::optional<int> score = std::nullopt) {
        Domain::SleepSession s;
        s.user_id = "1234567890";
        s.sleep_id = sleep_id;
        s.timezone = "Asia/Shanghai";
        s.start_at = "2026-09-23T23:00:00+08:00";
        s.end_at = "2026-09-24T07:00:00+08:00";
        s.collected_at = s.end_at;
        s.duration_minutes = 480;
        s.time_awake_minutes = 24;
        s.time_asleep_minutes = 456;
        s.sleep_score = score;
        if (score.has_value()) {
            s.sleep_score_source = "daily_report";
        }
        s.stages = {{"deep", 82}, {"light", 271}};
        return s;
    }

    static nlohmann::json row(const std::string& sleep_id) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(
                "SELECT COALESCE(sleep_score::text, 'null'), "
                "COALESCE(sleep_score_source, 'null'), stages::text, "
                "(SELECT COUNT(*) FROM sleep_sessions) "
                "FROM sleep_sessions WHERE sleep_id = $1",
                sleep_id);
            return nlohmann::json{{"score", r[0][0].template as<std::string>()},
                                  {"source", r[0][1].template as<std::string>()},
                                  {"stages", nlohmann::json::parse(r[0][2].template as<std::string>())},
                                  {"rows", r[0][3].template as<long>()}};
        });
    }
};

}  // namespace

TEST_F(SleepRepositoryTest, UpsertIsIdempotent) {
    Repositories::SleepRepository repo;
    const auto first = repo.upsert({session("s1"), session("s2")});
    EXPECT_EQ(first.added, 2);
    const auto second = repo.upsert({session("s1"), session("s2")});
    EXPECT_EQ(second.updated, 2);
    EXPECT_EQ(row("s1")["rows"], 2);
}

// Правило 5 нормализации: провал необязательного запроса отчётов не стирает
// ранее известную оценку тех же границ. Повторный синк приносит ту же сессию
// без оценки, и COALESCE в upsert сохраняет известную.
TEST_F(SleepRepositoryTest, KnownScoreSurvivesAScorelessResync) {
    Repositories::SleepRepository repo;
    repo.upsert({session("s1", 78)});
    repo.upsert({session("s1")});

    const auto r = row("s1");
    EXPECT_EQ(r["score"], "78");
    EXPECT_EQ(r["source"], "daily_report");
}

TEST_F(SleepRepositoryTest, FreshScoreReplacesTheOldOne) {
    Repositories::SleepRepository repo;
    repo.upsert({session("s1", 78)});
    repo.upsert({session("s1", 81)});
    EXPECT_EQ(row("s1")["score"], "81");
}

TEST_F(SleepRepositoryTest, StagesRoundTripThroughJsonb) {
    Repositories::SleepRepository repo;
    repo.upsert({session("s1")});
    const auto stages = row("s1")["stages"];
    ASSERT_EQ(stages.size(), 2u);
    EXPECT_EQ(stages[0]["stage"], "deep");
    EXPECT_EQ(stages[0]["minutes"], 82);
}
