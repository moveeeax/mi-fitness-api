/**
 * @file test_normalize_rest.cpp
 * @brief Нормализация пяти оставшихся типов: тренировки, вес, пульс, SpO2,
 *        стресс, аномалии ритма. Ноль от сервера это NULL, не измерение.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "FakeHttpTransport.hpp"
#include "xiaomi/CloudClient.hpp"
#include "xiaomi/Normalize.hpp"

namespace {

using nlohmann::json;

constexpr long long kNoon = 1790049600;  // 2026-09-22T12:00:00+08:00

json record(long long epoch, json value, json extra = json::object()) {
    json out{{"time", epoch}, {"zone_offset", 28800}, {"zone_name", "Asia/Shanghai"}, {"value", value.dump()}};
    out.update(extra);
    return out;
}

}  // namespace

TEST(NormalizeWorkouts, EndTimeFallsBackToStartPlusDuration) {
    long skipped = 0;
    const auto workouts = Xiaomi::normalize_workouts(
        {record(
            kNoon,
            json{{"start_time", kNoon}, {"duration", 1800}, {"distance", 5000}, {"calories", 320}, {"avg_hrm", 141}},
            json{{"key", "running"}, {"sid", "watch-1"}})},
        "1234567890",
        skipped);

    ASSERT_EQ(workouts.size(), 1u);
    const auto& w = workouts[0];
    EXPECT_EQ(w.activity_type, "running");
    EXPECT_EQ(w.duration_minutes, 30);
    EXPECT_EQ(w.end_at, "2026-09-22T12:30:00+08:00");
    EXPECT_DOUBLE_EQ(w.distance_m.value(), 5000.0);
    EXPECT_EQ(w.avg_heart_rate_bpm.value(), 141);
    // Не присланные метрики это NULL, не ноль.
    EXPECT_FALSE(w.max_heart_rate_bpm.has_value());
    EXPECT_FALSE(w.total_steps.has_value());
}

TEST(NormalizeWorkouts, ZeroMetricsBecomeNull) {
    long skipped = 0;
    const auto workouts = Xiaomi::normalize_workouts(
        {record(
            kNoon,
            json{{"start_time", kNoon}, {"end_time", kNoon + 600}, {"distance", 0}, {"calories", 0}, {"avg_hrm", 0}})},
        "1234567890",
        skipped);
    ASSERT_EQ(workouts.size(), 1u);
    EXPECT_FALSE(workouts[0].distance_m.has_value());
    EXPECT_FALSE(workouts[0].calories_kcal.has_value());
    EXPECT_FALSE(workouts[0].avg_heart_rate_bpm.has_value());
}

TEST(NormalizeBody, RecordWithoutWeightIsDroppedEntirely) {
    long skipped = 0;
    const auto rows =
        Xiaomi::normalize_body({record(kNoon, json{{"bmi", 31.8}}),  // только BMI, веса нет: не измерение
                                record(kNoon + 60, json{{"weight", 91.9}, {"bmi", 31.8}, {"visceral_fat", 14}})},
                               "1234567890",
                               skipped);

    ASSERT_EQ(rows.size(), 1u);
    EXPECT_DOUBLE_EQ(rows[0].weight_kg, 91.9);
    EXPECT_EQ(rows[0].visceral_fat_score.value(), 14);
}

TEST(NormalizeBody, ZeroCompositionFieldsBecomeNull) {
    long skipped = 0;
    const auto rows = Xiaomi::normalize_body(
        {record(kNoon, json{{"weight", 91.9}, {"bmi", 0}, {"body_fat_rate", 0}, {"basal_metabolism", 0}})},
        "1234567890",
        skipped);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(rows[0].bmi.has_value());
    EXPECT_FALSE(rows[0].body_fat_pct.has_value());
    EXPECT_FALSE(rows[0].basal_metabolism_kcal.has_value());
}

TEST(NormalizeHeartRate, TypeZeroIsPassiveOthersActive) {
    long skipped = 0;
    const auto samples = Xiaomi::normalize_heart_rate(
        {record(kNoon, json{{"bpm", 62}, {"type", 0}}), record(kNoon + 60, json{{"bpm", 118}, {"type", 1}})},
        {},
        "1234567890",
        skipped);

    ASSERT_EQ(samples.size(), 2u);
    EXPECT_EQ(samples[0].sample_type, "passive");
    EXPECT_EQ(samples[1].sample_type, "active");
    EXPECT_EQ(samples[1].bpm, 118);
}

TEST(NormalizeHeartRate, RestingComesFromItsOwnKeyWithDateTime) {
    long skipped = 0;
    const auto samples = Xiaomi::normalize_heart_rate(
        {}, {record(kNoon, json{{"bpm", 54}, {"date_time", kNoon - 3600}})}, "1234567890", skipped);

    ASSERT_EQ(samples.size(), 1u);
    EXPECT_EQ(samples[0].sample_type, "resting");
    EXPECT_EQ(samples[0].timestamp, "2026-09-22T11:00:00+08:00");
}

TEST(NormalizeSpo2, ValueKeyIsAFallbackAndRecordsWithoutItAreDropped) {
    long skipped = 0;
    const auto samples = Xiaomi::normalize_spo2({record(kNoon, json{{"spo2", 97}}),
                                                 record(kNoon + 60, json{{"value", 95}}),
                                                 record(kNoon + 120, json::object())},
                                                "1234567890",
                                                skipped);

    ASSERT_EQ(samples.size(), 2u);
    EXPECT_EQ(samples[0].spo2_pct, 97);
    EXPECT_EQ(samples[1].spo2_pct, 95);
}

TEST(NormalizeStress, LevelsFollowTheThresholds) {
    long skipped = 0;
    const auto samples = Xiaomi::normalize_stress({record(kNoon, json{{"stress", 29}}),
                                                   record(kNoon + 60, json{{"score", 30}}),
                                                   record(kNoon + 120, json{{"value", 60}})},
                                                  "1234567890",
                                                  skipped);

    ASSERT_EQ(samples.size(), 3u);
    EXPECT_EQ(samples[0].level, "low");
    EXPECT_EQ(samples[1].level, "medium");
    EXPECT_EQ(samples[2].level, "high");
}

TEST(NormalizeAbnormal, EndFallsBackToStart) {
    long skipped = 0;
    const auto events = Xiaomi::normalize_abnormal_heart_beat(
        {record(kNoon, json{{"start_time", kNoon}, {"duration", 45}})}, "1234567890", skipped);

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].start_at, events[0].end_at);
    EXPECT_EQ(events[0].duration_seconds.value(), 45);
}

// Тренировки идут отдельным эндпоинтом с limit 50 и полем sport_records.
TEST(FetchSportRecords, UsesItsOwnEndpointAndField) {
    FakeHttpTransport transport;
    transport.reply_login();
    transport.reply_encrypted(R"({"code":0,"result":{"sport_records":[{"a":1}],"has_more":true,"next_key":"s1"}})");
    transport.reply_encrypted(R"({"code":0,"result":{"sport_records":[{"a":2}],"has_more":false}})");
    Xiaomi::CloudClient client(transport, {"1234567890", std::string(347, 'S'), "cn"}, [](const auto&) {});
    client.login();

    const auto items = client.fetch_sport_records("2026-09-22", "2026-09-23");

    EXPECT_EQ(items.size(), 2u);
    EXPECT_NE(transport.requests().back().url.find("get_sport_records_by_time"), std::string::npos);
}
