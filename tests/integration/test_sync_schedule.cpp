/**
 * @file test_sync_schedule.cpp
 * @brief Постановка планового синка: окно последних суток в поясе региона.
 *
 * Таймер сам по себе это runEvery дрогона, его не тестируем; проверяется
 * функция, которую он дёргает: правильные даты окна, строка журнала queued
 * и задание в очереди.
 */

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "jobs/Jobs.hpp"
#include "jobs/XiaomiSyncHandler.hpp"
#include "repositories/SyncRunRepository.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;

namespace {

class SyncScheduleTest : public TestHelpers::CoreBackedTest {
protected:
    std::string config_file_name() const override { return "sync_schedule_test_config.json"; }

    void config_overrides(nlohmann::json& cfg) override {
        cfg["jobs"]["enabled"] = true;
        cfg["jobs"]["result_ttl"] = 3600;
    }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE sync_runs");
            return true;
        });
    }
};

}  // namespace

TEST_F(SyncScheduleTest, EnqueueRecentCoversTheWindow) {
    // 2026-09-22 01:00 +08:00.
    const long run_id = Jobs::XiaomiSync::enqueue_recent(2, 1790006400 + 3600);

    Repositories::SyncRunRepository runs;
    const auto row = runs.get(run_id);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ((*row)["status"], "queued");
    EXPECT_EQ((*row)["requested_start"], "2026-09-21");
    EXPECT_EQ((*row)["requested_end"], "2026-09-22");

    auto job = Jobs::get().pick({"xiaomi_sync"}, 1);
    ASSERT_TRUE(job.has_value());
    EXPECT_EQ(job->payload["run_id"].get<long>(), run_id);
    EXPECT_EQ(job->payload["from"], "2026-09-21");
    EXPECT_EQ(job->payload["to"], "2026-09-22");
}

TEST_F(SyncScheduleTest, WindowDatesFollowTheRegionZone) {
    // 1790006400 = 2026-09-22 00:00 +08:00, по UTC ещё 21-е: сутки должны
    // считаться в поясе региона (cn по умолчанию), а не по UTC.
    const long run_id = Jobs::XiaomiSync::enqueue_recent(1, 1790006400);

    Repositories::SyncRunRepository runs;
    const auto row = runs.get(run_id);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ((*row)["requested_start"], "2026-09-22");
    EXPECT_EQ((*row)["requested_end"], "2026-09-22");
    // Очередь не оставляем занятой для соседних тестов.
    (void)Jobs::get().pick({"xiaomi_sync"}, 1);
}
