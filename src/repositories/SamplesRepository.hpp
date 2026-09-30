/**
 * @file SamplesRepository.hpp
 * @brief Идемпотентный upsert точечных замеров: пульс, SpO2, стресс, аномалии.
 *
 * Пульс различает passive/active/resting в ключе уникальности: замер покоя и
 * пассивный замер одной секунды это две разные строки.
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "database/Database.hpp"
#include "domain/Health.hpp"
#include "repositories/ActivityRepository.hpp"  // UpsertCounts

namespace Repositories {

class SamplesRepository {
public:
    UpsertCounts upsert_heart_rate(const std::vector<Domain::HeartRateSample>& samples) {
        UpsertCounts counts;
        Database::get().execute_write([&](auto& txn) {
            for (const auto& s : samples) {
                auto r = txn.exec_params(
                    "INSERT INTO heart_rate_samples "
                    "(user_id, timestamp, timezone, collected_at, source_record_id, bpm, "
                    " sample_type, updated_at) "
                    "VALUES ($1, $2, $3, $4, $5, $6, $7, now()) "
                    "ON CONFLICT (user_id, timestamp, sample_type) DO UPDATE SET "
                    "bpm = EXCLUDED.bpm, updated_at = now() "
                    "RETURNING (xmax = 0) AS inserted",
                    s.user_id,
                    s.timestamp,
                    s.timezone,
                    s.collected_at.empty() ? std::optional<std::string>() : std::optional<std::string>(s.collected_at),
                    s.source_record_id.empty() ? std::optional<std::string>()
                                               : std::optional<std::string>(s.source_record_id),
                    s.bpm,
                    s.sample_type);
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

    UpsertCounts upsert_spo2(const std::vector<Domain::Spo2Sample>& samples) {
        UpsertCounts counts;
        Database::get().execute_write([&](auto& txn) {
            for (const auto& s : samples) {
                auto r = txn.exec_params(
                    "INSERT INTO spo2_samples "
                    "(user_id, timestamp, timezone, collected_at, source_record_id, spo2_pct, "
                    " updated_at) "
                    "VALUES ($1, $2, $3, $4, $5, $6, now()) "
                    "ON CONFLICT (user_id, timestamp) DO UPDATE SET "
                    "spo2_pct = EXCLUDED.spo2_pct, updated_at = now() "
                    "RETURNING (xmax = 0) AS inserted",
                    s.user_id,
                    s.timestamp,
                    s.timezone,
                    s.collected_at.empty() ? std::optional<std::string>() : std::optional<std::string>(s.collected_at),
                    s.source_record_id.empty() ? std::optional<std::string>()
                                               : std::optional<std::string>(s.source_record_id),
                    s.spo2_pct);
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

    UpsertCounts upsert_stress(const std::vector<Domain::StressSample>& samples) {
        UpsertCounts counts;
        Database::get().execute_write([&](auto& txn) {
            for (const auto& s : samples) {
                auto r = txn.exec_params(
                    "INSERT INTO stress_samples "
                    "(user_id, timestamp, timezone, collected_at, source_record_id, "
                    " stress_score, level, updated_at) "
                    "VALUES ($1, $2, $3, $4, $5, $6, $7, now()) "
                    "ON CONFLICT (user_id, timestamp) DO UPDATE SET "
                    "stress_score = EXCLUDED.stress_score, level = EXCLUDED.level, "
                    "updated_at = now() "
                    "RETURNING (xmax = 0) AS inserted",
                    s.user_id,
                    s.timestamp,
                    s.timezone,
                    s.collected_at.empty() ? std::optional<std::string>() : std::optional<std::string>(s.collected_at),
                    s.source_record_id.empty() ? std::optional<std::string>()
                                               : std::optional<std::string>(s.source_record_id),
                    s.stress_score,
                    s.level);
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

    UpsertCounts upsert_abnormal(const std::vector<Domain::AbnormalHeartBeatEvent>& events) {
        UpsertCounts counts;
        Database::get().execute_write([&](auto& txn) {
            for (const auto& e : events) {
                auto r = txn.exec_params(
                    "INSERT INTO abnormal_heart_beat_events "
                    "(user_id, event_id, timezone, collected_at, source_record_id, start_at, "
                    " end_at, duration_seconds, updated_at) "
                    "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, now()) "
                    "ON CONFLICT (user_id, event_id) DO UPDATE SET "
                    "start_at = EXCLUDED.start_at, end_at = EXCLUDED.end_at, "
                    "duration_seconds = EXCLUDED.duration_seconds, updated_at = now() "
                    "RETURNING (xmax = 0) AS inserted",
                    e.user_id,
                    e.event_id,
                    e.timezone,
                    e.collected_at.empty() ? std::optional<std::string>() : std::optional<std::string>(e.collected_at),
                    e.source_record_id.empty() ? std::optional<std::string>()
                                               : std::optional<std::string>(e.source_record_id),
                    e.start_at,
                    e.end_at,
                    e.duration_seconds);
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
