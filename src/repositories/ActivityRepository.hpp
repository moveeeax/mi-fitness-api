/**
 * @file ActivityRepository.hpp
 * @brief Идемпотентный upsert суточной активности (миграция 012).
 *
 * Вставка и обновление различаются трюком RETURNING (xmax = 0): у свежей
 * строки xmax нулевой, у обновлённой нет. Один проход, без предварительного
 * SELECT.
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "database/Database.hpp"
#include "domain/Health.hpp"

namespace Repositories {

struct UpsertCounts {
    long added = 0;
    long updated = 0;
};

class ActivityRepository {
public:
    UpsertCounts upsert(const std::vector<Domain::DailyActivity>& days) {
        UpsertCounts counts;
        if (days.empty()) {
            return counts;
        }
        Database::get().execute_write([&](auto& txn) {
            for (const auto& day : days) {
                // Пустая строка времени это NULL: отсутствие измерения не
                // притворяется значением (правило 2 нормализации).
                const std::optional<std::string> collected =
                    day.collected_at.empty() ? std::nullopt : std::optional<std::string>(day.collected_at);
                auto r = txn.exec_params(
                    "INSERT INTO daily_activity "
                    "(user_id, device_id, date, steps, distance_m, active_kcal, timezone, "
                    " collected_at, updated_at) "
                    "VALUES ($1, '', $2, $3, $4, $5, $6, $7, now()) "
                    "ON CONFLICT (user_id, date, device_id) DO UPDATE SET "
                    "steps = EXCLUDED.steps, distance_m = EXCLUDED.distance_m, "
                    "active_kcal = EXCLUDED.active_kcal, timezone = EXCLUDED.timezone, "
                    "collected_at = EXCLUDED.collected_at, updated_at = now() "
                    "RETURNING (xmax = 0) AS inserted",
                    day.user_id,
                    day.date,
                    day.steps,
                    day.distance_m,
                    day.active_kcal,
                    day.timezone,
                    collected);
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
