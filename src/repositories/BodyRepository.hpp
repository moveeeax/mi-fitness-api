/**
 * @file BodyRepository.hpp
 * @brief Идемпотентный upsert замеров тела (миграция 012).
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "database/Database.hpp"
#include "domain/Health.hpp"
#include "repositories/ActivityRepository.hpp"  // UpsertCounts

namespace Repositories {

class BodyRepository {
public:
    UpsertCounts upsert(const std::vector<Domain::BodyMeasurement>& rows) {
        UpsertCounts counts;
        Database::get().execute_write([&](auto& txn) {
            for (const auto& b : rows) {
                auto r = txn.exec_params(
                    "INSERT INTO body_measurements "
                    "(user_id, device_id, timestamp, timezone, collected_at, weight_kg, bmi, "
                    " body_fat_pct, muscle_mass_kg, water_pct, bone_mass_kg, "
                    " visceral_fat_score, basal_metabolism_kcal, metabolic_age, updated_at) "
                    "VALUES ($1, '', $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, now()) "
                    "ON CONFLICT (user_id, timestamp, device_id) DO UPDATE SET "
                    "weight_kg = EXCLUDED.weight_kg, bmi = EXCLUDED.bmi, "
                    "body_fat_pct = EXCLUDED.body_fat_pct, "
                    "muscle_mass_kg = EXCLUDED.muscle_mass_kg, water_pct = EXCLUDED.water_pct, "
                    "bone_mass_kg = EXCLUDED.bone_mass_kg, "
                    "visceral_fat_score = EXCLUDED.visceral_fat_score, "
                    "basal_metabolism_kcal = EXCLUDED.basal_metabolism_kcal, "
                    "metabolic_age = EXCLUDED.metabolic_age, updated_at = now() "
                    "RETURNING (xmax = 0) AS inserted",
                    b.user_id,
                    b.timestamp,
                    b.timezone,
                    b.collected_at.empty() ? std::optional<std::string>() : std::optional<std::string>(b.collected_at),
                    b.weight_kg,
                    b.bmi,
                    b.body_fat_pct,
                    b.muscle_mass_kg,
                    b.water_pct,
                    b.bone_mass_kg,
                    b.visceral_fat_score,
                    b.basal_metabolism_kcal,
                    b.metabolic_age);
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
