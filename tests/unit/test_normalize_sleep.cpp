/**
 * @file test_normalize_sleep.cpp
 * @brief Нормализация сна и суточная оценка: правила 3-5 спеки дословно.
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

// 2026-09-24: сон 23:00 +08:00 до 07:00 +08:00 следующего утра.
constexpr long long kBed = 1790175600;   // 2026-09-23T23:00:00+08:00
constexpr long long kWake = 1790204400;  // 2026-09-24T07:00:00+08:00

json sleep_record(long long bed, long long wake, json extra = json::object(), const std::string& sid = "band-1") {
    json value{{"bedtime", bed}, {"wake_up_time", wake}};
    value.update(extra);
    return json{
        {"time", wake}, {"zone_offset", 28800}, {"zone_name", "Asia/Shanghai"}, {"sid", sid}, {"value", value.dump()}};
}

json report(long long day_time, int score, json segments, const std::string& sid = "band-1") {
    json value{{"score", score}};
    if (!segments.is_null()) {
        value["segment_details"] = segments;
    }
    return json{{"time", day_time},
                {"zone_offset", 28800},
                {"key", "sleep"},
                {"tag", "daily_report"},
                {"sid", sid},
                {"value", value.dump()}};
}

std::vector<Domain::SleepSession> normalize(const std::vector<json>& records) {
    long skipped = 0;
    return Xiaomi::normalize_sleep(records, "1234567890", skipped);
}

}  // namespace

TEST(NormalizeSleep, FieldsFollowThePriorityChains) {
    const auto sessions =
        normalize({sleep_record(kBed, kWake, json{{"duration", 470}, {"awake_duration", 24}, {"is_nap", false}})});

    ASSERT_EQ(sessions.size(), 1u);
    const auto& s = sessions[0];
    EXPECT_EQ(s.start_epoch, kBed);
    EXPECT_EQ(s.end_epoch, kWake);
    EXPECT_EQ(s.duration_minutes, 470);
    EXPECT_EQ(s.time_awake_minutes, 24);
    EXPECT_EQ(s.time_asleep_minutes, 446);
    EXPECT_FALSE(s.is_nap);
    EXPECT_EQ(s.start_at, "2026-09-23T23:00:00+08:00");
}

TEST(NormalizeSleep, DurationFallsBackToBoundaries) {
    const auto sessions = normalize({sleep_record(kBed, kWake)});
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0].duration_minutes, 480);
}

TEST(NormalizeSleep, StagesFollowTheStateMapping) {
    const json items = json::array({
        json{{"start_time", kBed}, {"end_time", kBed + 600}, {"state", 2}},
        json{{"start_time", kBed + 600}, {"end_time", kBed + 1200}, {"state", 4}},
        json{{"start_time", kBed + 1200}, {"end_time", kBed + 1800}, {"state", 99}},
        json{{"start_time", kBed + 1800}, {"end_time", kBed + 1800}, {"state", 5}},  // ноль минут
    });
    const auto sessions = normalize({sleep_record(kBed, kWake, json{{"items", items}})});

    ASSERT_EQ(sessions.size(), 1u);
    const auto& stages = sessions[0].stages;
    ASSERT_EQ(stages.size(), 3u);
    EXPECT_EQ(stages[0].stage, "deep");
    EXPECT_EQ(stages[1].stage, "rem");
    EXPECT_EQ(stages[2].stage, "light");  // неизвестный код это light
}

TEST(NormalizeSleep, RecordScoreCarriesProvenanceZeroAndFractionAreNull) {
    const auto with_score = normalize({sleep_record(kBed, kWake, json{{"score", 86}})});
    ASSERT_EQ(with_score.size(), 1u);
    EXPECT_EQ(with_score[0].sleep_score.value(), 86);
    EXPECT_EQ(with_score[0].sleep_score_source.value(), "sleep_record");

    // Ноль это «нет данных», дробное это невалидный балл (правила эталона).
    EXPECT_FALSE(normalize({sleep_record(kBed, kWake, json{{"score", 0}})})[0].sleep_score.has_value());
    EXPECT_FALSE(normalize({sleep_record(kBed, kWake, json{{"score", 86.5}})})[0].sleep_score.has_value());
}

TEST(NormalizeSleep, MissingBothBoundariesSkipsTheRecord) {
    long skipped = 0;
    const auto sessions = Xiaomi::normalize_sleep(
        {json{{"time", kWake}, {"zone_offset", 28800}, {"value", "{}"}}}, "1234567890", skipped);
    EXPECT_TRUE(sessions.empty());
}

TEST(ApplySleepScores, SegmentBoundariesPickExactlyOneSession) {
    auto sessions = normalize({
        sleep_record(kBed, kWake),                                // главный сон
        sleep_record(kWake + 3 * 3600, kWake + 3 * 3600 + 1800),  // обрывок днём
    });
    const json segments = json::array({json{{"bedtime", kBed}, {"wake_up_time", kWake}},
                                       json{{"bedtime", kWake + 3 * 3600}, {"wake_up_time", kWake + 3 * 3600 + 1800}}});
    Xiaomi::apply_daily_sleep_scores(sessions, {report(kWake, 78, segments)}, 28800);

    ASSERT_EQ(sessions.size(), 2u);
    EXPECT_EQ(sessions[0].sleep_score.value(), 78);
    EXPECT_EQ(sessions[0].sleep_score_source.value(), "daily_report");
    EXPECT_FALSE(sessions[1].sleep_score.has_value());
}

TEST(ApplySleepScores, AmbiguousEqualLongestGetsNothing) {
    // Две главные сессии одной длины и одного источника, отчёт без сегментов:
    // выбрать нельзя, оценка не ставится никому.
    auto sessions = normalize({sleep_record(kBed, kWake), sleep_record(kBed - 90000, kWake - 90000)});
    Xiaomi::apply_daily_sleep_scores(sessions, {report(kWake, 70, json()), report(kWake - 90000, 70, json())}, 28800);
    // Даты пробуждения разные, здесь по одному кандидату на отчёт: оба получают.
    EXPECT_TRUE(sessions[0].sleep_score.has_value());
    EXPECT_TRUE(sessions[1].sleep_score.has_value());

    auto same_day = normalize({sleep_record(kBed, kWake, json{{"duration", 480}}),
                               sleep_record(kBed + 60, kWake + 60, json{{"duration", 480}})});
    Xiaomi::apply_daily_sleep_scores(same_day, {report(kWake, 70, json())}, 28800);
    EXPECT_FALSE(same_day[0].sleep_score.has_value());
    EXPECT_FALSE(same_day[1].sleep_score.has_value());
}

TEST(ApplySleepScores, TwoDevicesWithoutSegmentsGetNothing) {
    auto sessions = normalize({sleep_record(kBed, kWake, json::object(), "band-1"),
                               sleep_record(kBed + 60, kWake, json::object(), "phone-2")});
    Xiaomi::apply_daily_sleep_scores(sessions, {report(kWake, 65, json(), "")}, 28800);
    EXPECT_FALSE(sessions[0].sleep_score.has_value());
    EXPECT_FALSE(sessions[1].sleep_score.has_value());
}

TEST(ApplySleepScores, ConflictingScoresStayUnavailable) {
    auto sessions = normalize({sleep_record(kBed, kWake)});
    const json segments = json::array({json{{"bedtime", kBed}, {"wake_up_time", kWake}}});
    Xiaomi::apply_daily_sleep_scores(sessions, {report(kWake, 70, segments), report(kWake + 60, 80, segments)}, 28800);
    EXPECT_FALSE(sessions[0].sleep_score.has_value());
}

TEST(ApplySleepScores, InvalidSegmentDiscardsTheReportNotTheSessions) {
    auto sessions = normalize({sleep_record(kBed, kWake)});
    const json broken = json::array({json{{"bedtime", kWake}, {"wake_up_time", kBed}}});
    Xiaomi::apply_daily_sleep_scores(sessions, {report(kWake, 70, broken)}, 28800);
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_FALSE(sessions[0].sleep_score.has_value());
}

TEST(ApplySleepScores, RecordScoreIsNeverOverwritten) {
    auto sessions = normalize({sleep_record(kBed, kWake, json{{"score", 86}})});
    const json segments = json::array({json{{"bedtime", kBed}, {"wake_up_time", kWake}}});
    Xiaomi::apply_daily_sleep_scores(sessions, {report(kWake, 55, segments)}, 28800);
    EXPECT_EQ(sessions[0].sleep_score.value(), 86);
    EXPECT_EQ(sessions[0].sleep_score_source.value(), "sleep_record");
}

TEST(ApplySleepScores, NapNeverReceivesTheDailyScore) {
    auto sessions = normalize({sleep_record(kBed, kWake, json{{"is_nap", true}})});
    Xiaomi::apply_daily_sleep_scores(sessions, {report(kWake, 70, json())}, 28800);
    EXPECT_FALSE(sessions[0].sleep_score.has_value());
}

// Отчёты идут своим эндпоинтом со строгим курсором: нестроковый, пустой или
// повторный это ошибка протокола, а не тихий обрыв.
TEST(FetchSleepReports, StrictCursorRules) {
    FakeHttpTransport transport;
    transport.reply_login();
    transport.reply_encrypted(R"({"code":0,"result":{"data_list":[{"a":1}],"has_more":true,"next_key":123}})");
    Xiaomi::CloudClient client(transport, {"1234567890", std::string(347, 'S'), "cn"}, [](const auto&) {});
    client.login();
    EXPECT_THROW(client.fetch_daily_sleep_reports("2026-09-23", "2026-09-25"), Xiaomi::MiFitnessProtocolError);
}

TEST(FetchSleepReports, TwoPagesConcatenate) {
    FakeHttpTransport transport;
    transport.reply_login();
    transport.reply_encrypted(R"({"code":0,"result":{"data_list":[{"a":1}],"has_more":true,"next_key":"r1"}})");
    transport.reply_encrypted(R"({"code":0,"result":{"data_list":[{"a":2}],"has_more":false}})");
    Xiaomi::CloudClient client(transport, {"1234567890", std::string(347, 'S'), "cn"}, [](const auto&) {});
    client.login();
    EXPECT_EQ(client.fetch_daily_sleep_reports("2026-09-23", "2026-09-25").size(), 2u);
}
