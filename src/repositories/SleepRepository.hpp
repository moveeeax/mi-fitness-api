/**
 * @file SleepRepository.hpp
 * @brief Идемпотентный upsert сессий сна с неприкосновенной оценкой.
 *
 * COALESCE(EXCLUDED.sleep_score, старое) реализует правило 5 нормализации:
 * провал необязательного запроса отчётов при повторном синке не стирает ранее
 * известную оценку тех же границ. Свежая валидная оценка заменяет старую.
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "database/Database.hpp"
#include "domain/Health.hpp"
#include "repositories/ActivityRepository.hpp"  // UpsertCounts

namespace Repositories {

class SleepRepository {
public:
    UpsertCounts upsert(const std::vector<Domain::SleepSession>& sessions) {
        UpsertCounts counts;
        if (sessions.empty()) {
            return counts;
        }
        Database::get().execute_write([&](auto& txn) {
            for (const auto& s : sessions) {
                const nlohmann::json stages = s.stages;
                auto r = txn.exec_params(
                    "INSERT INTO sleep_sessions "
                    "(user_id, sleep_id, source_record_id, timezone, collected_at, start_at, "
                    " end_at, duration_minutes, time_asleep_minutes, time_awake_minutes, "
                    " sleep_score, sleep_score_source, is_nap, stages, updated_at) "
                    "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, "
                    "        $14::jsonb, now()) "
                    "ON CONFLICT (user_id, sleep_id) DO UPDATE SET "
                    "source_record_id = EXCLUDED.source_record_id, timezone = EXCLUDED.timezone, "
                    "collected_at = EXCLUDED.collected_at, start_at = EXCLUDED.start_at, "
                    "end_at = EXCLUDED.end_at, duration_minutes = EXCLUDED.duration_minutes, "
                    "time_asleep_minutes = EXCLUDED.time_asleep_minutes, "
                    "time_awake_minutes = EXCLUDED.time_awake_minutes, "
                    "sleep_score = COALESCE(EXCLUDED.sleep_score, sleep_sessions.sleep_score), "
                    "sleep_score_source = COALESCE(EXCLUDED.sleep_score_source, "
                    "                              sleep_sessions.sleep_score_source), "
                    "is_nap = EXCLUDED.is_nap, stages = EXCLUDED.stages, updated_at = now() "
                    "RETURNING (xmax = 0) AS inserted",
                    s.user_id,
                    s.sleep_id,
                    s.source_record_id.empty() ? std::optional<std::string>()
                                               : std::optional<std::string>(s.source_record_id),
                    s.timezone,
                    s.collected_at.empty() ? std::optional<std::string>() : std::optional<std::string>(s.collected_at),
                    s.start_at,
                    s.end_at,
                    s.duration_minutes,
                    s.time_asleep_minutes,
                    s.time_awake_minutes,
                    s.sleep_score,
                    s.sleep_score_source,
                    s.is_nap,
                    stages.dump());
                if (r[0][0].template as<bool>()) {
                    ++counts.added;
                } else {
                    ++counts.updated;
                }
            }
            return true;
        });
        return counts;
    }
};

}  // namespace Repositories
