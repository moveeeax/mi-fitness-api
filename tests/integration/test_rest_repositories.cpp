/**
 * @file test_rest_repositories.cpp
 * @brief Идемпотентный upsert пяти оставшихся типов.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "database/Database.hpp"
#include "repositories/BodyRepository.hpp"
#include "repositories/SamplesRepository.hpp"
#include "repositories/WorkoutRepository.hpp"
#include "test_helpers.hpp"

namespace {

class RestRepositoriesTest : public TestHelpers::CoreBackedTest {
protected:
    std::string config_file_name() const override { return "rest_repos_test_config.json"; }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Database::get().execute_write([](auto& txn) {
            txn.exec(
                "TRUNCATE TABLE workouts, body_measurements, heart_rate_samples, spo2_samples, "
                "stress_samples, abnormal_heart_beat_events");
            return true;
        });
    }

    static long count_rows(const std::string& table) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec("SELECT COUNT(*) FROM " + table);
            return r[0][0].template as<long>();
        });
    }
};

Domain::Workout workout() {
    Domain::Workout w;
    w.user_id = "1234567890";
    w.workout_id = "watch-1_running_1790049600";
    w.activity_type = "running";
    w.start_at = "2026-09-22T12:00:00+08:00";
    w.end_at = "2026-09-22T12:30:00+08:00";
    w.duration_minutes = 30;
    w.distance_m = 5000.0;
    return w;
}

Domain::BodyMeasurement body() {
    Domain::BodyMeasurement b;
    b.user_id = "1234567890";
    b.timestamp = "2026-09-22T10:38:12+08:00";
    b.weight_kg = 91.9;
    b.visceral_fat_score = 14;
    return b;
}

Domain::HeartRateSample hr(const std::string& type, int bpm) {
    Domain::HeartRateSample s;
    s.user_id = "1234567890";
    s.timestamp = "2026-09-22T12:00:00+08:00";
    s.bpm = bpm;
    s.sample_type = type;
    return s;
}

}  // namespace

TEST_F(RestRepositoriesTest, WorkoutUpsertIsIdempotent) {
    Repositories::WorkoutRepository repo;
    EXPECT_EQ(repo.upsert({workout()}).added, 1);
    EXPECT_EQ(repo.upsert({workout()}).updated, 1);
    EXPECT_EQ(count_rows("workouts"), 1);
}

TEST_F(RestRepositoriesTest, BodyUpsertKeepsNulls) {
    Repositories::BodyRepository repo;
    EXPECT_EQ(repo.upsert({body()}).added, 1);
    EXPECT_EQ(repo.upsert({body()}).updated, 1);

    const bool bmi_null = Database::get().execute_read([](auto& txn) {
        auto r = txn.exec("SELECT bmi IS NULL FROM body_measurements");
        return r[0][0].template as<bool>();
    });
    EXPECT_TRUE(bmi_null);
}

// Пассивный замер и замер покоя в одну и ту же секунду это две строки:
// sample_type входит в ключ уникальности.
TEST_F(RestRepositoriesTest, SampleTypeSeparatesHeartRateRows) {
    Repositories::SamplesRepository repo;
    EXPECT_EQ(repo.upsert_heart_rate({hr("passive", 62), hr("resting", 54)}).added, 2);
    EXPECT_EQ(repo.upsert_heart_rate({hr("passive", 63)}).updated, 1);
    EXPECT_EQ(count_rows("heart_rate_samples"), 2);
}

TEST_F(RestRepositoriesTest, PointSamplesAreIdempotent) {
    Repositories::SamplesRepository repo;

    Domain::Spo2Sample o2;
    o2.user_id = "1234567890";
    o2.timestamp = "2026-09-22T03:00:00+08:00";
    o2.spo2_pct = 97;
    EXPECT_EQ(repo.upsert_spo2({o2}).added, 1);
    EXPECT_EQ(repo.upsert_spo2({o2}).updated, 1);

    Domain::StressSample st;
    st.user_id = "1234567890";
    st.timestamp = "2026-09-22T12:00:00+08:00";
    st.stress_score = 42;
    st.level = "medium";
    EXPECT_EQ(repo.upsert_stress({st}).added, 1);

    Domain::AbnormalHeartBeatEvent ev;
    ev.user_id = "1234567890";
    ev.event_id = "1790049600";
    ev.start_at = "2026-09-22T12:00:00+08:00";
    ev.end_at = ev.start_at;
    EXPECT_EQ(repo.upsert_abnormal({ev}).added, 1);
    EXPECT_EQ(repo.upsert_abnormal({ev}).updated, 1);
    EXPECT_EQ(count_rows("abnormal_heart_beat_events"), 1);
}
