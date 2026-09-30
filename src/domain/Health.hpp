/**
 * @file Health.hpp
 * @brief Доменные структуры восьми типов данных Mi Fitness.
 *
 * Времена храним строками ISO с зоной, как отдаёт нормализация: Postgres
 * принимает их в timestamptz без посредников, а сравнение с эталоном идёт
 * по секундам эпохи. Отсутствующее измерение это std::optional, ноль от
 * сервера сюда не попадает (правило 2 нормализации).
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Domain {

struct DailyActivity {
    std::string user_id;
    std::string date;  // YYYY-MM-DD локальных суток записи
    std::string timezone = "UTC";
    std::string collected_at;  // ISO, пусто = NULL
    long steps = 0;
    std::optional<double> distance_m;
    std::optional<double> active_kcal;
};

struct SleepStage {
    std::string stage;  // deep | light | rem | awake
    int minutes = 0;
};

struct SleepSession {
    std::string user_id;
    std::string sleep_id;
    std::string source_record_id;
    std::string timezone = "UTC";
    std::string collected_at;
    std::string start_at;
    std::string end_at;
    int duration_minutes = 0;
    int time_asleep_minutes = 0;
    int time_awake_minutes = 0;
    std::optional<int> sleep_score;
    std::optional<std::string> sleep_score_source;  // sleep_record | daily_report
    bool is_nap = false;
    std::vector<SleepStage> stages;
    /// sid записи облака: участвует в сопоставлении суточной оценки.
    std::optional<std::string> source_sid;
    /// Секунды эпохи границ: сегменты отчёта сравниваются по ним точно.
    std::int64_t start_epoch = 0;
    std::int64_t end_epoch = 0;
};

struct Workout {
    std::string user_id;
    std::string workout_id;
    std::string source_record_id;
    std::string activity_type;
    std::string timezone = "UTC";
    std::string collected_at;
    std::string start_at;
    std::string end_at;
    int duration_minutes = 0;
    std::optional<double> distance_m;
    std::optional<double> calories_kcal;
    std::optional<int> avg_heart_rate_bpm;
    std::optional<int> max_heart_rate_bpm;
    std::optional<double> avg_pace_sec_per_km;
    std::optional<double> max_pace_sec_per_km;
    std::optional<int> total_steps;
};

struct BodyMeasurement {
    std::string user_id;
    std::string timestamp;
    std::string timezone = "UTC";
    std::string collected_at;
    double weight_kg = 0;  // запись без веса не существует: она пропускается
    std::optional<double> bmi;
    std::optional<double> body_fat_pct;
    std::optional<double> muscle_mass_kg;
    std::optional<double> water_pct;
    std::optional<double> bone_mass_kg;
    std::optional<int> visceral_fat_score;
    std::optional<int> basal_metabolism_kcal;
    std::optional<int> metabolic_age;
};

struct HeartRateSample {
    std::string user_id;
    std::string timestamp;
    std::string timezone = "UTC";
    std::string collected_at;
    std::string source_record_id;
    int bpm = 0;
    std::string sample_type;  // passive | active | resting
};

struct Spo2Sample {
    std::string user_id;
    std::string timestamp;
    std::string timezone = "UTC";
    std::string collected_at;
    std::string source_record_id;
    int spo2_pct = 0;
};

struct StressSample {
    std::string user_id;
    std::string timestamp;
    std::string timezone = "UTC";
    std::string collected_at;
    std::string source_record_id;
    int stress_score = 0;
    std::string level;  // low | medium | high
};

struct AbnormalHeartBeatEvent {
    std::string user_id;
    std::string event_id;
    std::string timezone = "UTC";
    std::string collected_at;
    std::string source_record_id;
    std::string start_at;
    std::string end_at;
    std::optional<int> duration_seconds;
};

inline void to_json(nlohmann::json& j, const SleepStage& s) {
    j = nlohmann::json{{"stage", s.stage}, {"minutes", s.minutes}};
}

}  // namespace Domain
