/**
 * @file WorkoutRepository.hpp
 * @brief Идемпотентный upsert тренировок (миграция 012).
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "database/Database.hpp"
#include "domain/Health.hpp"
#include "repositories/ActivityRepository.hpp"  // UpsertCounts

namespace Repositories {

class WorkoutRepository {
public:
    UpsertCounts upsert(const std::vector<Domain::Workout>& workouts) {
        UpsertCounts counts;
        Database::get().execute_write([&](auto& txn) {
            for (const auto& w : workouts) {
                auto r = txn.exec_params(
                    "INSERT INTO workouts "
                    "(user_id, workout_id, source_record_id, activity_type, timezone, "
                    " collected_at, start_at, end_at, duration_minutes, distance_m, "
                    " calories_kcal, avg_heart_rate_bpm, max_heart_rate_bpm, "
                    " avg_pace_sec_per_km, max_pace_sec_per_km, total_steps, updated_at) "
                    "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, "
                    "        $16, now()) "
                    "ON CONFLICT (user_id, workout_id) DO UPDATE SET "
                    "activity_type = EXCLUDED.activity_type, start_at = EXCLUDED.start_at, "
                    "end_at = EXCLUDED.end_at, duration_minutes = EXCLUDED.duration_minutes, "
                    "distance_m = EXCLUDED.distance_m, calories_kcal = EXCLUDED.calories_kcal, "
                    "avg_heart_rate_bpm = EXCLUDED.avg_heart_rate_bpm, "
                    "max_heart_rate_bpm = EXCLUDED.max_heart_rate_bpm, "
                    "avg_pace_sec_per_km = EXCLUDED.avg_pace_sec_per_km, "
                    "max_pace_sec_per_km = EXCLUDED.max_pace_sec_per_km, "
                    "total_steps = EXCLUDED.total_steps, updated_at = now() "
                    "RETURNING (xmax = 0) AS inserted",
                    w.user_id,
                    w.workout_id,
                    w.source_record_id.empty() ? std::optional<std::string>()
                                               : std::optional<std::string>(w.source_record_id),
                    w.activity_type,
                    w.timezone,
                    w.collected_at.empty() ? std::optional<std::string>() : std::optional<std::string>(w.collected_at),
                    w.start_at,
                    w.end_at,
                    w.duration_minutes,
                    w.distance_m,
                    w.calories_kcal,
                    w.avg_heart_rate_bpm,
                    w.max_heart_rate_bpm,
                    w.avg_pace_sec_per_km,
                    w.max_pace_sec_per_km,
                    w.total_steps);
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
