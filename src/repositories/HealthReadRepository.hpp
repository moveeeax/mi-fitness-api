/**
 * @file HealthReadRepository.hpp
 * @brief Чтение данных здоровья для маршрутов чтения данных.
 *
 * JSON собирает Postgres (json_agg поверх подзапроса): строки не гоняются
 * через C++ поштучно, репозиторий разбирает одну ячейку. Времена наружу в
 * ISO UTC c суффиксом +00:00; границы диапазона это сутки UTC, сравнение
 * идёт по timestamptz без приведения к date, чтобы работали индексы
 * миграции 016.
 */

#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "database/Database.hpp"

namespace Repositories {

class HealthReadRepository {
public:
    struct Page {
        nlohmann::json rows = nlohmann::json::array();
        long total = 0;
    };

    Page daily_activity(const std::string& from, const std::string& to, long limit, long offset) {
        return page(
            "SELECT date::text, steps, distance_m, active_kcal, total_kcal, timezone "
            "FROM daily_activity WHERE date BETWEEN $1 AND $2 ORDER BY date",
            "SELECT COUNT(*) FROM daily_activity WHERE date BETWEEN $1 AND $2",
            from,
            to,
            limit,
            offset);
    }

    Page sleep(const std::string& from, const std::string& to, long limit, long offset) {
        return page("SELECT sleep_id, " + iso("start_at") + " AS start_at, " + iso("end_at") +
                        " AS end_at, duration_minutes, time_asleep_minutes, time_awake_minutes, "
                        "sleep_score, sleep_score_source, is_nap, timezone, stages "
                        "FROM sleep_sessions WHERE " +
                        day_range("end_at") + " ORDER BY end_at",
                    "SELECT COUNT(*) FROM sleep_sessions WHERE " + day_range("end_at"),
                    from,
                    to,
                    limit,
                    offset);
    }

    Page heart_rate(const std::string& from, const std::string& to, const std::string& type, long limit, long offset) {
        // Пустой type это все типы; фильтр входит параметром, не конкатенацией
        // значения. Номера плейсхолдеров в выборке и счётчике разные: у
        // выборки $3/$4 заняты limit/offset.
        return page("SELECT " + iso("timestamp") +
                        " AS timestamp, bpm, sample_type "
                        "FROM heart_rate_samples WHERE " +
                        day_range("timestamp") + " AND ($5 = '' OR sample_type = $5) ORDER BY timestamp",
                    "SELECT COUNT(*) FROM heart_rate_samples WHERE " + day_range("timestamp") +
                        " AND ($3 = '' OR sample_type = $3)",
                    from,
                    to,
                    limit,
                    offset,
                    type);
    }

    Page stress(const std::string& from, const std::string& to, long limit, long offset) {
        return page("SELECT " + iso("timestamp") +
                        " AS timestamp, stress_score, level "
                        "FROM stress_samples WHERE " +
                        day_range("timestamp") + " ORDER BY timestamp",
                    "SELECT COUNT(*) FROM stress_samples WHERE " + day_range("timestamp"),
                    from,
                    to,
                    limit,
                    offset);
    }

    Page spo2(const std::string& from, const std::string& to, long limit, long offset) {
        return page("SELECT " + iso("timestamp") +
                        " AS timestamp, spo2_pct "
                        "FROM spo2_samples WHERE " +
                        day_range("timestamp") + " ORDER BY timestamp",
                    "SELECT COUNT(*) FROM spo2_samples WHERE " + day_range("timestamp"),
                    from,
                    to,
                    limit,
                    offset);
    }

    Page body(const std::string& from, const std::string& to, long limit, long offset) {
        return page("SELECT " + iso("timestamp") +
                        " AS timestamp, weight_kg, bmi, body_fat_pct, muscle_mass_kg, "
                        "water_pct, bone_mass_kg, visceral_fat_score, basal_metabolism_kcal, "
                        "metabolic_age FROM body_measurements WHERE " +
                        day_range("timestamp") + " ORDER BY timestamp",
                    "SELECT COUNT(*) FROM body_measurements WHERE " + day_range("timestamp"),
                    from,
                    to,
                    limit,
                    offset);
    }

    Page workouts(const std::string& from, const std::string& to, long limit, long offset) {
        return page("SELECT workout_id, activity_type, " + iso("start_at") + " AS start_at, " + iso("end_at") +
                        " AS end_at, duration_minutes, distance_m, calories_kcal, "
                        "avg_heart_rate_bpm, max_heart_rate_bpm, avg_pace_sec_per_km, "
                        "max_pace_sec_per_km, total_steps FROM workouts WHERE " +
                        day_range("start_at") + " ORDER BY start_at",
                    "SELECT COUNT(*) FROM workouts WHERE " + day_range("start_at"),
                    from,
                    to,
                    limit,
                    offset);
    }

    Page summary(
        const std::string& from, const std::string& to, long limit, long offset, long long zone_offset_seconds) {
        // date у активности локальная (пояс устройства), поэтому окна сна и
        // resting-пульса строятся от локальной полуночи в поясе региона:
        // UTC-окно уводило утренние записи в предыдущую строку (обзор фазы
        // 3, Important 5). Остаточный сдвиг пояса устройства против пояса
        // региона даёт край в час — записан Ruling-ом.
        const std::string day_start =
            "(a.date::timestamp AT TIME ZONE 'UTC' - make_interval(secs => $5::double precision))";
        const std::string select =
            "SELECT a.date::text, a.steps, a.distance_m, a.active_kcal, "
            "s.duration_minutes AS sleep_duration_minutes, s.sleep_score, hr.bpm AS resting_bpm "
            "FROM daily_activity a "
            "LEFT JOIN LATERAL (SELECT duration_minutes, sleep_score FROM sleep_sessions "
            " WHERE NOT is_nap AND end_at >= " +
            day_start + " AND end_at < " + day_start +
            " + interval '1 day' ORDER BY duration_minutes DESC LIMIT 1) s ON true "
            "LEFT JOIN LATERAL (SELECT bpm FROM heart_rate_samples "
            " WHERE sample_type = 'resting' AND timestamp >= " +
            day_start + " AND timestamp < " + day_start +
            " + interval '1 day' "
            " ORDER BY timestamp DESC LIMIT 1) hr ON true "
            "WHERE a.date BETWEEN $1 AND $2 ORDER BY a.date";
        return page(select,
                    "SELECT COUNT(*) FROM daily_activity WHERE date BETWEEN $1 AND $2 AND $3::bigint > -86401",
                    from,
                    to,
                    limit,
                    offset,
                    zone_offset_seconds);
    }

    Page abnormal_heart_beat(const std::string& from, const std::string& to, long limit, long offset) {
        return page("SELECT event_id, " + iso("start_at") + " AS start_at, " + iso("end_at") +
                        " AS end_at, duration_seconds FROM abnormal_heart_beat_events WHERE " + day_range("start_at") +
                        " ORDER BY start_at",
                    "SELECT COUNT(*) FROM abnormal_heart_beat_events WHERE " + day_range("start_at"),
                    from,
                    to,
                    limit,
                    offset);
    }

    /// Границы и счётчики по каждому типу плюс время последнего синка.
    nlohmann::json coverage() {
        nlohmann::json out = nlohmann::json::object();
        Database::get().execute_read([&](auto& txn) {
            const struct {
                const char* type;
                const char* table;
                const char* column;
            } kTables[] = {
                {"daily_activity", "daily_activity", "date"},
                {"sleep", "sleep_sessions", "end_at"},
                {"heart_rate", "heart_rate_samples", "timestamp"},
                {"stress", "stress_samples", "timestamp"},
                {"spo2", "spo2_samples", "timestamp"},
                {"body_measurements", "body_measurements", "timestamp"},
                {"workouts", "workouts", "start_at"},
                {"abnormal_heart_beat", "abnormal_heart_beat_events", "start_at"},
            };
            for (const auto& e : kTables) {
                const std::string col(e.column);
                // Для timestamptz дата берётся в UTC, не в поясе сессии.
                const std::string day = col == "date" ? col : "(" + col + " AT TIME ZONE 'UTC')";
                auto r = txn.exec(
                    "SELECT json_build_object("
                    "'first_date', MIN(" +
                    day +
                    ")::date::text, "
                    "'last_date', MAX(" +
                    day +
                    ")::date::text, "
                    "'records', COUNT(*)) FROM " +
                    std::string(e.table));
                out[e.type] = nlohmann::json::parse(r[0][0].template as<std::string>());
            }
            auto s = txn.exec(
                "SELECT COALESCE(json_object_agg(data_type, to_char(last_sync_at AT TIME ZONE 'UTC', "
                "'YYYY-MM-DD\"T\"HH24:MI:SS\"+00:00\"')), '{}'::json) FROM sync_state");
            const auto last = nlohmann::json::parse(s[0][0].template as<std::string>());
            for (auto& [type, entry] : out.items()) {
                entry["last_sync_at"] = last.contains(type) ? last[type] : nlohmann::json();
            }
            return 0;
        });
        return out;
    }

    /// Все строки типа за диапазон, без пагинации: страница экспорта.
    nlohmann::json export_rows(const std::string& type, const std::string& from, const std::string& to) {
        const long kNoLimit = 100000000;
        if (type == "daily_activity")
            return daily_activity(from, to, kNoLimit, 0).rows;
        if (type == "sleep")
            return sleep(from, to, kNoLimit, 0).rows;
        if (type == "heart_rate")
            return heart_rate(from, to, "", kNoLimit, 0).rows;
        if (type == "stress")
            return stress(from, to, kNoLimit, 0).rows;
        if (type == "spo2")
            return spo2(from, to, kNoLimit, 0).rows;
        if (type == "body_measurements")
            return body(from, to, kNoLimit, 0).rows;
        if (type == "workouts")
            return workouts(from, to, kNoLimit, 0).rows;
        if (type == "abnormal_heart_beat")
            return abnormal_heart_beat(from, to, kNoLimit, 0).rows;
        return nlohmann::json::array();
    }

private:
    /// ISO UTC с явным +00:00: timestamptz наружу без потери смысла.
    static std::string iso(const std::string& column) {
        return "to_char(" + column + " AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"+00:00\"')";
    }

    /// Сутки UTC поверх timestamptz: полуинтервал, дружественный индексу.
    /// Явный AT TIME ZONE 'UTC': приведение date -> timestamptz берёт
    /// полночь в поясе сессии Postgres, а ответ обещает UTC (обзор фазы 3,
    /// Important 4; боевая база в Etc/UTC, правка защитная).
    static std::string day_range(const std::string& column) {
        return "(" + column + " >= $1::date::timestamp AT TIME ZONE 'UTC' AND " + column +
               " < ($2::date + 1)::timestamp AT TIME ZONE 'UTC')";
    }

    template <typename... Extra>
    Page page(const std::string& select,
              const std::string& count_sql,
              const std::string& from,
              const std::string& to,
              long limit,
              long offset,
              Extra&&... extra) {
        Page out;
        Database::get().execute_read([&](auto& txn) {
            auto agg =
                txn.exec_params("SELECT COALESCE(json_agg(t), '[]'::json) FROM (" + select + " LIMIT $3 OFFSET $4) t",
                                from,
                                to,
                                limit,
                                offset,
                                extra...);
            out.rows = nlohmann::json::parse(agg[0][0].template as<std::string>());
            auto total = txn.exec_params(count_sql, from, to, std::forward<Extra>(extra)...);
            out.total = total[0][0].template as<long>();
            return 0;
        });
        return out;
    }
};

}  // namespace Repositories
