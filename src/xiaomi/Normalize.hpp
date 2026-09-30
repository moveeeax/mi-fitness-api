/**
 * @file Normalize.hpp
 * @brief Сырые записи облака → доменные структуры. Чистые функции.
 *
 * Ни сети, ни базы: вход это JSON-записи вида {time, zone_offset, zone_name,
 * sid, value}, выход доменные структуры. Битая запись увеличивает skipped и
 * никогда не роняет тип целиком. Семантика повторяет эталонный Python-адаптер
 * дословно, любое отклонение это дефект.
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "domain/Health.hpp"

namespace Xiaomi {

/// Метки времени до 2000 года это мусор устройства, а не данные.
inline constexpr std::int64_t kMinValidTimestamp = 946684800;

struct ActivityResult {
    std::vector<Domain::DailyActivity> days;
    long suppressed_steps = 0;
    long skipped = 0;
};

/**
 * @brief Суточная активность из минутных срезов шагов и отдельного ключа калорий.
 *
 * Группировка по локальной минуте записи. При коллизии минуты (телефон и
 * браслет шлют параллельные срезы одной активности) остаётся запись с большим
 * кортежем (шаги, дистанция, калории): сумма дала бы двойной счёт, наблюдённое
 * завышение у эталона +73..+208 шагов в день. Подавленные шаги считаются.
 * Калории ключа calories замещают суммированные из шагов по датам, где есть.
 */
ActivityResult normalize_daily_activity(const std::vector<nlohmann::json>& step_records,
                                        const std::vector<nlohmann::json>& calorie_records,
                                        std::string_view user_id);

namespace detail {

/// Локальные секунды эпохи записи: time + zone_offset. Бросает на битом или
/// доисторическом времени.
std::int64_t record_epoch(const nlohmann::json& item);

/// value приходит и строкой с JSON внутри, и готовым объектом.
nlohmann::json parse_value(const nlohmann::json& item);

/// ISO-строка с явным смещением: 2026-09-22T10:00:00+08:00.
std::string iso_with_offset(std::int64_t epoch, int offset_seconds);

}  // namespace detail

}  // namespace Xiaomi
