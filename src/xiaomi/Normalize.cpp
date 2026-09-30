/**
 * @file Normalize.cpp
 * @brief Тела нормализации.
 */

#include "xiaomi/Normalize.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <tuple>
#include <utility>

#include "xiaomi/Errors.hpp"

namespace Xiaomi {

namespace detail {

std::int64_t record_epoch(const nlohmann::json& item) {
    std::int64_t timestamp = 0;
    const auto& raw = item.at("time");
    if (raw.is_number_integer()) {
        timestamp = raw.get<std::int64_t>();
    } else if (raw.is_string()) {
        timestamp = std::stoll(raw.get<std::string>());
    } else {
        throw MiFitnessProtocolError("record has no valid time field");
    }
    if (timestamp < kMinValidTimestamp) {
        throw MiFitnessProtocolError("record time predates year 2000");
    }
    return timestamp;
}

int zone_offset_of(const nlohmann::json& item) {
    if (!item.contains("zone_offset") || item["zone_offset"].is_null()) {
        return 0;
    }
    const auto& raw = item["zone_offset"];
    if (raw.is_number()) {
        return raw.get<int>();
    }
    if (raw.is_string() && !raw.get<std::string>().empty()) {
        return std::stoi(raw.get<std::string>());
    }
    return 0;
}

nlohmann::json parse_value(const nlohmann::json& item) {
    if (!item.contains("value")) {
        return nlohmann::json::object();
    }
    const auto& raw = item["value"];
    if (raw.is_object()) {
        return raw;
    }
    if (raw.is_string()) {
        auto parsed = nlohmann::json::parse(raw.get<std::string>(),
                                            nullptr,
                                            /*allow_exceptions=*/false);
        if (parsed.is_object()) {
            return parsed;
        }
    }
    throw MiFitnessProtocolError("record value is not a JSON object");
}

std::string iso_with_offset(std::int64_t epoch, int offset_seconds) {
    using namespace std::chrono;
    const sys_seconds local{seconds{epoch + offset_seconds}};
    const auto days = floor<std::chrono::days>(local);
    const year_month_day ymd{days};
    const hh_mm_ss<seconds> tod{local - days};
    const char sign = offset_seconds < 0 ? '-' : '+';
    const int abs_offset = offset_seconds < 0 ? -offset_seconds : offset_seconds;
    char out[40];
    std::snprintf(out,
                  sizeof(out),
                  "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
                  static_cast<int>(ymd.year()),
                  static_cast<unsigned>(ymd.month()),
                  static_cast<unsigned>(ymd.day()),
                  static_cast<int>(tod.hours().count()),
                  static_cast<int>(tod.minutes().count()),
                  static_cast<int>(tod.seconds().count()),
                  sign,
                  abs_offset / 3600,
                  (abs_offset % 3600) / 60);
    return out;
}

std::string local_date(std::int64_t epoch, int offset_seconds) {
    return iso_with_offset(epoch, offset_seconds).substr(0, 10);
}

double number_or_zero(const nlohmann::json& payload, const char* key) {
    if (!payload.contains(key) || payload[key].is_null()) {
        return 0.0;
    }
    const auto& value = payload[key];
    if (value.is_number()) {
        return value.get<double>();
    }
    if (value.is_string()) {
        try {
            return std::stod(value.get<std::string>());
        } catch (...) {
            return 0.0;
        }
    }
    return 0.0;
}

}  // namespace detail

ActivityResult normalize_daily_activity(const std::vector<nlohmann::json>& step_records,
                                        const std::vector<nlohmann::json>& calorie_records,
                                        std::string_view user_id) {
    using detail::iso_with_offset;
    using detail::local_date;
    using detail::number_or_zero;

    struct Minute {
        long steps = 0;
        double distance = 0;
        double calories = 0;
        std::int64_t epoch = 0;
        int offset = 0;
        std::string zone_name;
    };
    struct Day {
        std::map<std::int64_t, Minute> minutes;  // ключ: локальная минута эпохи
    };

    ActivityResult result;
    std::map<std::string, Day> days;

    for (const auto& item : step_records) {
        try {
            const std::int64_t epoch = detail::record_epoch(item);
            const int offset = detail::zone_offset_of(item);
            const auto payload = detail::parse_value(item);

            Minute candidate;
            candidate.steps = static_cast<long>(number_or_zero(payload, "steps"));
            candidate.distance = number_or_zero(payload, "distance");
            candidate.calories = number_or_zero(payload, "calories");
            candidate.epoch = epoch;
            candidate.offset = offset;
            candidate.zone_name =
                item.value("zone_name", std::string()).empty() ? "UTC" : item["zone_name"].get<std::string>();

            const std::int64_t minute_key = (epoch + offset) / 60;
            auto& day = days[local_date(epoch, offset)];
            const auto found = day.minutes.find(minute_key);
            if (found == day.minutes.end()) {
                day.minutes.emplace(minute_key, std::move(candidate));
                continue;
            }
            // Параллельные записи одной минуты это одна и та же активность:
            // остаётся согласованная запись с большим кортежем, а не сумма.
            Minute& existing = found->second;
            result.suppressed_steps += std::min(existing.steps, candidate.steps);
            const auto as_tuple = [](const Minute& m) { return std::make_tuple(m.steps, m.distance, m.calories); };
            if (as_tuple(candidate) > as_tuple(existing)) {
                existing = std::move(candidate);
            }
        } catch (const std::exception&) {
            ++result.skipped;
        }
    }

    struct DayTotals {
        long steps = 0;
        double distance = 0;
        double calories = 0;
        std::int64_t max_epoch = 0;
        int offset = 0;
        std::string zone_name = "UTC";
    };
    std::map<std::string, DayTotals> totals;
    for (const auto& [date, day] : days) {
        DayTotals& t = totals[date];
        for (const auto& [minute_key, minute] : day.minutes) {
            t.steps += minute.steps;
            t.distance += minute.distance;
            t.calories += minute.calories;
            if (!minute.zone_name.empty()) {
                t.zone_name = minute.zone_name;
            }
            if (minute.epoch > t.max_epoch) {
                t.max_epoch = minute.epoch;
                t.offset = minute.offset;
            }
        }
    }

    // Ключ calories облака замещает суммированные из шагов калории по датам,
    // где он есть: это отдельный, более точный ряд.
    std::map<std::string, double> calorie_totals;
    for (const auto& item : calorie_records) {
        try {
            const std::int64_t epoch = detail::record_epoch(item);
            const int offset = detail::zone_offset_of(item);
            const auto payload = detail::parse_value(item);
            const std::string date = local_date(epoch, offset);
            calorie_totals[date] += number_or_zero(payload, "calories");
            DayTotals& t = totals[date];
            if (epoch > t.max_epoch) {
                t.max_epoch = epoch;
                t.offset = offset;
            }
            const std::string zone = item.value("zone_name", std::string());
            if (!zone.empty()) {
                t.zone_name = zone;
            }
        } catch (const std::exception&) {
            ++result.skipped;
        }
    }
    for (const auto& [date, calories] : calorie_totals) {
        totals[date].calories = calories;
    }

    for (const auto& [date, t] : totals) {
        Domain::DailyActivity day;
        day.user_id = std::string(user_id);
        day.date = date;
        day.timezone = t.zone_name;
        day.collected_at = t.max_epoch > 0 ? iso_with_offset(t.max_epoch, t.offset) : "";
        day.steps = t.steps;
        day.distance_m = t.distance;
        day.active_kcal = t.calories;
        result.days.push_back(std::move(day));
    }
    return result;
}

}  // namespace Xiaomi
