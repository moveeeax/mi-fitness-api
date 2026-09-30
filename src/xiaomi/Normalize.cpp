/**
 * @file Normalize.cpp
 * @brief Тела нормализации.
 */

#include "xiaomi/Normalize.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <set>
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

namespace Xiaomi {

namespace {

using detail::iso_with_offset;

/// Балл валиден только целым в (0, 100]: нули это «нет данных» у моделей
/// Xiaomi, дробное это мусор. Порядок ключей как у эталона.
std::optional<int> valid_sleep_score(const nlohmann::json& payload) {
    for (const char* name : {"score", "sleep_score"}) {
        if (!payload.contains(name)) {
            continue;
        }
        const auto& value = payload[name];
        if (value.is_boolean() || value.is_null()) {
            continue;
        }
        double number = 0;
        if (value.is_number()) {
            number = value.get<double>();
        } else if (value.is_string()) {
            try {
                number = std::stod(value.get<std::string>());
            } catch (...) {
                continue;
            }
        } else {
            continue;
        }
        if (number > 0 && number <= 100 && number == static_cast<long long>(number)) {
            return static_cast<int>(number);
        }
    }
    return std::nullopt;
}

/// sid со значениями "" и "default" означает «источник неизвестен».
std::optional<std::string> sleep_source(const nlohmann::json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    const std::string text = value.is_string() ? value.get<std::string>() : value.dump();
    if (text.empty() || text == "default") {
        return std::nullopt;
    }
    return text;
}

std::int64_t field_epoch(const nlohmann::json& payload, const char* key) {
    const auto& value = payload.at(key);
    std::int64_t out = 0;
    if (value.is_number()) {
        out = value.get<std::int64_t>();
    } else if (value.is_string()) {
        out = std::stoll(value.get<std::string>());
    } else {
        throw MiFitnessProtocolError("timestamp field has an unexpected type");
    }
    if (out < kMinValidTimestamp) {
        throw MiFitnessProtocolError("timestamp predates year 2000");
    }
    return out;
}

std::optional<std::int64_t> first_epoch(const nlohmann::json& payload, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        if (payload.contains(key) && !payload[key].is_null()) {
            const auto& v = payload[key];
            const bool falsy =
                (v.is_number() && v.get<double>() == 0) || (v.is_string() && v.get<std::string>().empty());
            if (!falsy) {
                return field_epoch(payload, key);
            }
        }
    }
    return std::nullopt;
}

std::string sleep_stage_name(const nlohmann::json& state) {
    // 2 deep, 3 light, 4 rem, 5 awake; код 1 в наблюдениях не встречался,
    // неизвестное это light (та же оговорка, что у эталона).
    int code = -1;
    if (state.is_number()) {
        code = state.get<int>();
    } else if (state.is_string()) {
        try {
            code = std::stoi(state.get<std::string>());
        } catch (...) {
            code = -1;
        }
    }
    switch (code) {
        case 2:
            return "deep";
        case 4:
            return "rem";
        case 5:
            return "awake";
        case 3:
        default:
            return "light";
    }
}

bool main_sleep_candidate(const Domain::SleepSession& s) {
    const std::int64_t span = s.end_epoch - s.start_epoch;
    return !s.is_nap && s.duration_minutes > 0 && s.duration_minutes <= 1440 && span > 0 && span <= 86400;
}

}  // namespace

std::vector<Domain::SleepSession> normalize_sleep(const std::vector<nlohmann::json>& records,
                                                  std::string_view user_id,
                                                  long& skipped) {
    std::vector<Domain::SleepSession> sessions;
    for (const auto& item : records) {
        try {
            const auto payload = detail::parse_value(item);
            const int offset = detail::zone_offset_of(item);

            const auto start = first_epoch(payload, {"bedtime", "device_bedtime", "bed_timestamp"});
            auto end = first_epoch(payload, {"wake_up_time", "device_wake_up_time", "out_bed_timestamp"});
            if (!end.has_value() && item.contains("time") && !item["time"].is_null()) {
                end = field_epoch(item, "time");
            }
            if (!start.has_value() || !end.has_value()) {
                continue;  // запись без границ это не сон, пропуск без skipped, как у эталона
            }

            Domain::SleepSession s;
            s.user_id = std::string(user_id);
            s.start_epoch = *start;
            s.end_epoch = *end;
            s.start_at = iso_with_offset(*start, offset);
            s.end_at = iso_with_offset(*end, offset);
            const long computed = std::max<std::int64_t>(0, (*end - *start) / 60);
            const double duration_raw =
                payload.contains("duration") && payload["duration"].is_number() ? payload["duration"].get<double>() : 0;
            s.duration_minutes = duration_raw > 0 ? static_cast<int>(duration_raw) : static_cast<int>(computed);
            double awake = 0;
            for (const char* key : {"awake_duration", "sleep_awake_duration"}) {
                if (payload.contains(key) && payload[key].is_number() && payload[key].get<double>() > 0) {
                    awake = payload[key].get<double>();
                    break;
                }
            }
            s.time_awake_minutes = static_cast<int>(awake);
            s.time_asleep_minutes = std::max(0, s.duration_minutes - s.time_awake_minutes);

            if (payload.contains("items") && payload["items"].is_array()) {
                for (const auto& segment : payload["items"]) {
                    try {
                        const std::int64_t seg_start = segment.value("start_time", 0LL);
                        const std::int64_t seg_end = segment.value("end_time", 0LL);
                        const int minutes = static_cast<int>(std::max<std::int64_t>(0, (seg_end - seg_start) / 60));
                        if (minutes > 0) {
                            s.stages.push_back({sleep_stage_name(segment.value("state", nlohmann::json())), minutes});
                        }
                    } catch (...) {
                        continue;
                    }
                }
            }

            const std::string sid_part =
                item.contains("sid") && item["sid"].is_string() && !item["sid"].get<std::string>().empty()
                    ? item["sid"].get<std::string>()
                    : std::string(user_id);
            const std::string time_part =
                item.contains("time") && !item["time"].is_null()
                    ? (item["time"].is_string() ? item["time"].get<std::string>()
                                                : std::to_string(item["time"].get<std::int64_t>()))
                    : std::to_string(*end);
            s.sleep_id = sid_part + "_" + time_part;
            s.source_record_id = item.contains("time") && !item["time"].is_null() ? time_part : "";
            s.timezone = item.value("zone_name", std::string()).empty() ? "UTC" : item["zone_name"].get<std::string>();
            s.collected_at = s.end_at;
            if (item.contains("time") && !item["time"].is_null()) {
                try {
                    s.collected_at = iso_with_offset(field_epoch(item, "time"), offset);
                } catch (...) {}
            }

            const auto score = valid_sleep_score(payload);
            s.sleep_score = score;
            if (score.has_value()) {
                s.sleep_score_source = "sleep_record";
            }
            const std::string nap = payload.contains("is_nap") ? payload["is_nap"].dump() : "false";
            std::string nap_lower = nap;
            std::transform(nap_lower.begin(), nap_lower.end(), nap_lower.begin(), ::tolower);
            s.is_nap = nap_lower == "true" || nap_lower == "1" || nap_lower == "\"true\"" || nap_lower == "\"1\"" ||
                       nap_lower == "\"yes\"";
            s.source_sid = sleep_source(item.contains("sid") ? item["sid"] : nlohmann::json());
            sessions.push_back(std::move(s));
        } catch (const std::exception&) {
            ++skipped;
        }
    }
    return sessions;
}

void apply_daily_sleep_scores(std::vector<Domain::SleepSession>& sessions,
                              const std::vector<nlohmann::json>& reports,
                              int default_zone_offset) {
    std::map<std::size_t, std::set<int>> proposals;
    for (const auto& item : reports) {
        try {
            if (!item.is_object()) {
                continue;
            }
            if (item.value("key", std::string("sleep")) != "sleep" ||
                item.value("tag", std::string("daily_report")) != "daily_report") {
                continue;
            }
            const auto payload = detail::parse_value(item);
            const auto score = valid_sleep_score(payload);
            if (!score.has_value()) {
                continue;
            }
            const int offset = item.contains("zone_offset") && item["zone_offset"].is_number()
                                   ? item["zone_offset"].get<int>()
                                   : default_zone_offset;
            const std::string report_day = detail::iso_with_offset(field_epoch(item, "time"), offset).substr(0, 10);
            auto source = sleep_source(item.contains("sid") ? item["sid"] : nlohmann::json());
            if (!source.has_value()) {
                source = sleep_source(payload.contains("did") ? payload["did"] : nlohmann::json());
            }

            std::vector<std::size_t> candidates;
            for (std::size_t i = 0; i < sessions.size(); ++i) {
                const auto& s = sessions[i];
                const std::string wake_day = s.end_at.substr(0, 10);
                if (main_sleep_candidate(s) && wake_day == report_day &&
                    (!source.has_value() || s.source_sid == source)) {
                    candidates.push_back(i);
                }
            }

            if (payload.contains("segment_details") && !payload["segment_details"].is_array()) {
                continue;
            }
            const auto segments = payload.value("segment_details", nlohmann::json::array());
            if (!segments.empty()) {
                // Суточный балл описывает главный (самый длинный) сегмент.
                // Любой невалидный сегмент делает весь отчёт непригодным.
                std::set<std::pair<std::int64_t, std::int64_t>> boundaries;
                for (const auto& segment : segments) {
                    const std::int64_t seg_start = field_epoch(segment, "bedtime");
                    const std::int64_t seg_end = field_epoch(segment, "wake_up_time");
                    if (!(seg_end - seg_start > 0 && seg_end - seg_start <= 86400)) {
                        throw MiFitnessProtocolError("invalid sleep report segment");
                    }
                    boundaries.emplace(seg_start, seg_end);
                }
                std::int64_t longest = 0;
                for (const auto& [seg_start, seg_end] : boundaries) {
                    longest = std::max(longest, seg_end - seg_start);
                }
                std::vector<std::pair<std::int64_t, std::int64_t>> main_segments;
                for (const auto& b : boundaries) {
                    if (b.second - b.first == longest) {
                        main_segments.push_back(b);
                    }
                }
                if (main_segments.size() != 1) {
                    continue;
                }
                const auto [seg_start, seg_end] = main_segments[0];
                std::vector<std::size_t> exact;
                for (const std::size_t i : candidates) {
                    if (sessions[i].start_epoch == seg_start && sessions[i].end_epoch == seg_end) {
                        exact.push_back(i);
                    }
                }
                candidates = exact;
            } else if (!candidates.empty()) {
                // Без границ между устройствами не выбираем: один источник и
                // единственная самая длинная сессия.
                std::set<std::optional<std::string>> sids;
                for (const std::size_t i : candidates) {
                    sids.insert(sessions[i].source_sid);
                }
                if (sids.size() != 1) {
                    continue;
                }
                int longest = 0;
                for (const std::size_t i : candidates) {
                    longest = std::max(longest, sessions[i].duration_minutes);
                }
                std::vector<std::size_t> longest_only;
                for (const std::size_t i : candidates) {
                    if (sessions[i].duration_minutes == longest) {
                        longest_only.push_back(i);
                    }
                }
                candidates = longest_only;
            }
            if (candidates.size() == 1) {
                proposals[candidates[0]].insert(*score);
            }
        } catch (const std::exception&) {
            // Один битый необязательный отчёт не отменяет остальные отчёты и
            // тем более сессии.
            continue;
        }
    }
    for (const auto& [index, scores] : proposals) {
        auto& session = sessions[index];
        if (!session.sleep_score.has_value() && scores.size() == 1) {
            session.sleep_score = *scores.begin();
            session.sleep_score_source = "daily_report";
        }
    }
}

}  // namespace Xiaomi

namespace Xiaomi {

namespace {

using detail::iso_with_offset;

std::optional<double> optional_number(const nlohmann::json& payload, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        if (!payload.contains(key) || payload[key].is_null()) {
            continue;
        }
        const auto& value = payload[key];
        double parsed = 0;
        if (value.is_number()) {
            parsed = value.get<double>();
        } else if (value.is_string()) {
            try {
                parsed = std::stod(value.get<std::string>());
            } catch (...) {
                continue;
            }
        } else {
            continue;
        }
        // Ноль от сервера это «нет данных», а не измеренный ноль.
        if (parsed != 0) {
            return parsed;
        }
    }
    return std::nullopt;
}

std::optional<int> optional_int(const nlohmann::json& payload, std::initializer_list<const char*> keys) {
    const auto value = optional_number(payload, keys);
    if (!value.has_value()) {
        return std::nullopt;
    }
    return static_cast<int>(*value);
}

std::string zone_name_of(const nlohmann::json& item) {
    const std::string zone = item.value("zone_name", std::string());
    return zone.empty() ? "UTC" : zone;
}

std::string record_id_of(const nlohmann::json& item) {
    if (!item.contains("time") || item["time"].is_null()) {
        return {};
    }
    return item["time"].is_string() ? item["time"].get<std::string>()
                                    : std::to_string(item["time"].get<std::int64_t>());
}

std::string collected_at_of(const nlohmann::json& item, int offset, const std::string& fallback) {
    try {
        return iso_with_offset(detail::record_epoch(item), offset);
    } catch (...) {
        return fallback;
    }
}

}  // namespace

std::vector<Domain::Workout> normalize_workouts(const std::vector<nlohmann::json>& records,
                                                std::string_view user_id,
                                                long& skipped) {
    std::vector<Domain::Workout> out;
    for (const auto& item : records) {
        try {
            const auto payload = detail::parse_value(item);
            const int offset = detail::zone_offset_of(item);

            std::optional<std::int64_t> start;
            if (payload.contains("start_time") && payload["start_time"].is_number() &&
                payload["start_time"].get<double>() != 0) {
                start = payload["start_time"].get<std::int64_t>();
            } else if (item.contains("time") && !item["time"].is_null()) {
                start = detail::record_epoch(item);
            }
            const long duration_seconds =
                payload.contains("duration") && payload["duration"].is_number() ? payload["duration"].get<long>() : 0;
            std::optional<std::int64_t> end;
            if (payload.contains("end_time") && payload["end_time"].is_number() &&
                payload["end_time"].get<double>() != 0) {
                end = payload["end_time"].get<std::int64_t>();
            } else if (start.has_value()) {
                end = *start + duration_seconds;
            }
            if (!start.has_value() || !end.has_value()) {
                continue;
            }
            if (*start < kMinValidTimestamp || *end < kMinValidTimestamp) {
                throw MiFitnessProtocolError("timestamp predates year 2000");
            }

            Domain::Workout w;
            w.user_id = std::string(user_id);
            w.start_at = iso_with_offset(*start, offset);
            w.end_at = iso_with_offset(*end, offset);
            w.duration_minutes = duration_seconds > 0
                                     ? static_cast<int>(duration_seconds / 60)
                                     : static_cast<int>(std::max<std::int64_t>(0, (*end - *start) / 60));
            const std::string sid =
                item.contains("sid") && item["sid"].is_string() && !item["sid"].get<std::string>().empty()
                    ? item["sid"].get<std::string>()
                    : std::string(user_id);
            const std::string kind = !item.value("category", std::string()).empty()
                                         ? item["category"].get<std::string>()
                                     : !item.value("key", std::string()).empty() ? item["key"].get<std::string>()
                                     : payload.contains("sport_type")            ? payload["sport_type"].dump()
                                                                                 : "workout";
            const std::string time_part = !record_id_of(item).empty() ? record_id_of(item) : std::to_string(*start);
            w.workout_id = sid + "_" +
                           (item.value("key", std::string()).empty() ? "workout" : item["key"].get<std::string>()) +
                           "_" + time_part;
            w.activity_type = kind;
            w.source_record_id = record_id_of(item);
            w.timezone = zone_name_of(item);
            w.collected_at = collected_at_of(item, offset, w.start_at);
            w.distance_m = optional_number(payload, {"distance"});
            w.calories_kcal = optional_number(payload, {"calories", "total_cal"});
            w.avg_heart_rate_bpm = optional_int(payload, {"avg_hrm"});
            w.max_heart_rate_bpm = optional_int(payload, {"max_hrm"});
            w.avg_pace_sec_per_km = optional_number(payload, {"avg_pace"});
            w.max_pace_sec_per_km = optional_number(payload, {"max_pace"});
            w.total_steps = optional_int(payload, {"steps", "total_steps"});
            out.push_back(std::move(w));
        } catch (const std::exception&) {
            ++skipped;
        }
    }
    return out;
}

std::vector<Domain::BodyMeasurement> normalize_body(const std::vector<nlohmann::json>& records,
                                                    std::string_view user_id,
                                                    long& skipped) {
    std::vector<Domain::BodyMeasurement> out;
    for (const auto& item : records) {
        try {
            const auto payload = detail::parse_value(item);
            const int offset = detail::zone_offset_of(item);
            const std::int64_t epoch = detail::record_epoch(item);

            // Запись без веса это «только постоял на весах»: не измерение.
            const auto weight = optional_number(payload, {"weight"});
            if (!weight.has_value()) {
                continue;
            }
            Domain::BodyMeasurement b;
            b.user_id = std::string(user_id);
            b.timestamp = iso_with_offset(epoch, offset);
            b.timezone = zone_name_of(item);
            b.collected_at = b.timestamp;
            b.weight_kg = *weight;
            b.bmi = optional_number(payload, {"bmi"});
            b.body_fat_pct = optional_number(payload, {"body_fat_rate"});
            b.muscle_mass_kg = optional_number(payload, {"muscle_rate"});
            b.water_pct = optional_number(payload, {"moisture_rate"});
            b.bone_mass_kg = optional_number(payload, {"bone_mass"});
            b.visceral_fat_score = optional_int(payload, {"visceral_fat"});
            b.basal_metabolism_kcal = optional_int(payload, {"basal_metabolism"});
            b.metabolic_age = optional_int(payload, {"body_age"});
            out.push_back(std::move(b));
        } catch (const std::exception&) {
            ++skipped;
        }
    }
    return out;
}

std::vector<Domain::HeartRateSample> normalize_heart_rate(const std::vector<nlohmann::json>& records,
                                                          const std::vector<nlohmann::json>& resting_records,
                                                          std::string_view user_id,
                                                          long& skipped) {
    std::vector<Domain::HeartRateSample> out;
    for (const auto& item : records) {
        try {
            const auto payload = detail::parse_value(item);
            const int offset = detail::zone_offset_of(item);
            const std::int64_t epoch = detail::record_epoch(item);

            Domain::HeartRateSample s;
            s.user_id = std::string(user_id);
            s.timestamp = iso_with_offset(epoch, offset);
            s.timezone = zone_name_of(item);
            s.collected_at = s.timestamp;
            s.source_record_id = record_id_of(item);
            s.bpm = payload.value("bpm", 0);
            const int type = payload.contains("type") && payload["type"].is_number() ? payload["type"].get<int>() : 0;
            s.sample_type = type == 0 ? "passive" : "active";
            out.push_back(std::move(s));
        } catch (const std::exception&) {
            ++skipped;
        }
    }
    for (const auto& item : resting_records) {
        try {
            const auto payload = detail::parse_value(item);
            const int offset = detail::zone_offset_of(item);
            std::int64_t epoch = 0;
            if (payload.contains("date_time") && payload["date_time"].is_number() &&
                payload["date_time"].get<double>() != 0) {
                epoch = payload["date_time"].get<std::int64_t>();
                if (epoch < kMinValidTimestamp) {
                    throw MiFitnessProtocolError("timestamp predates year 2000");
                }
            } else {
                epoch = detail::record_epoch(item);
            }
            Domain::HeartRateSample s;
            s.user_id = std::string(user_id);
            s.timestamp = iso_with_offset(epoch, offset);
            s.timezone = zone_name_of(item);
            s.collected_at = collected_at_of(item, offset, s.timestamp);
            s.source_record_id = record_id_of(item);
            s.bpm = payload.value("bpm", 0);
            s.sample_type = "resting";
            out.push_back(std::move(s));
        } catch (const std::exception&) {
            ++skipped;
        }
    }
    return out;
}

std::vector<Domain::Spo2Sample> normalize_spo2(const std::vector<nlohmann::json>& records,
                                               std::string_view user_id,
                                               long& skipped) {
    std::vector<Domain::Spo2Sample> out;
    for (const auto& item : records) {
        try {
            const auto payload = detail::parse_value(item);
            const int offset = detail::zone_offset_of(item);
            const auto value = optional_number(payload, {"spo2", "value"});
            if (!value.has_value()) {
                continue;
            }
            std::int64_t epoch = 0;
            if (payload.contains("time") && payload["time"].is_number() && payload["time"].get<double>() != 0) {
                epoch = payload["time"].get<std::int64_t>();
                if (epoch < kMinValidTimestamp) {
                    throw MiFitnessProtocolError("timestamp predates year 2000");
                }
            } else {
                epoch = detail::record_epoch(item);
            }
            Domain::Spo2Sample s;
            s.user_id = std::string(user_id);
            s.timestamp = iso_with_offset(epoch, offset);
            s.timezone = zone_name_of(item);
            s.collected_at = collected_at_of(item, offset, s.timestamp);
            s.source_record_id = record_id_of(item);
            s.spo2_pct = static_cast<int>(*value);
            out.push_back(std::move(s));
        } catch (const std::exception&) {
            ++skipped;
        }
    }
    return out;
}

std::vector<Domain::StressSample> normalize_stress(const std::vector<nlohmann::json>& records,
                                                   std::string_view user_id,
                                                   long& skipped) {
    std::vector<Domain::StressSample> out;
    for (const auto& item : records) {
        try {
            const auto payload = detail::parse_value(item);
            const int offset = detail::zone_offset_of(item);
            const auto value = optional_number(payload, {"stress", "score", "value"});
            if (!value.has_value()) {
                continue;
            }
            std::int64_t epoch = 0;
            if (payload.contains("time") && payload["time"].is_number() && payload["time"].get<double>() != 0) {
                epoch = payload["time"].get<std::int64_t>();
                if (epoch < kMinValidTimestamp) {
                    throw MiFitnessProtocolError("timestamp predates year 2000");
                }
            } else {
                epoch = detail::record_epoch(item);
            }
            Domain::StressSample s;
            s.user_id = std::string(user_id);
            s.timestamp = iso_with_offset(epoch, offset);
            s.timezone = zone_name_of(item);
            s.collected_at = collected_at_of(item, offset, s.timestamp);
            s.source_record_id = record_id_of(item);
            s.stress_score = static_cast<int>(*value);
            s.level = s.stress_score < 30 ? "low" : s.stress_score < 60 ? "medium" : "high";
            out.push_back(std::move(s));
        } catch (const std::exception&) {
            ++skipped;
        }
    }
    return out;
}

std::vector<Domain::AbnormalHeartBeatEvent> normalize_abnormal_heart_beat(const std::vector<nlohmann::json>& records,
                                                                          std::string_view user_id,
                                                                          long& skipped) {
    std::vector<Domain::AbnormalHeartBeatEvent> out;
    for (const auto& item : records) {
        try {
            const auto payload = detail::parse_value(item);
            const int offset = detail::zone_offset_of(item);

            std::optional<std::int64_t> start;
            if (payload.contains("start_time") && payload["start_time"].is_number() &&
                payload["start_time"].get<double>() != 0) {
                start = payload["start_time"].get<std::int64_t>();
            } else if (item.contains("time") && !item["time"].is_null()) {
                start = detail::record_epoch(item);
            }
            if (!start.has_value()) {
                continue;
            }
            std::int64_t end = *start;
            if (payload.contains("end_time") && payload["end_time"].is_number() &&
                payload["end_time"].get<double>() != 0) {
                end = payload["end_time"].get<std::int64_t>();
            }
            if (*start < kMinValidTimestamp || end < kMinValidTimestamp) {
                throw MiFitnessProtocolError("timestamp predates year 2000");
            }

            Domain::AbnormalHeartBeatEvent e;
            e.user_id = std::string(user_id);
            e.event_id = !record_id_of(item).empty() ? record_id_of(item) : std::to_string(*start);
            e.start_at = iso_with_offset(*start, offset);
            e.end_at = iso_with_offset(end, offset);
            e.timezone = zone_name_of(item);
            e.collected_at = collected_at_of(item, offset, e.start_at);
            e.source_record_id = record_id_of(item);
            e.duration_seconds = optional_int(payload, {"duration"});
            out.push_back(std::move(e));
        } catch (const std::exception&) {
            ++skipped;
        }
    }
    return out;
}

}  // namespace Xiaomi
