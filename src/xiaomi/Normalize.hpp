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

/**
 * @brief Сессии сна из сырых записей ключа sleep.
 *
 * Приоритеты полей, маппинг стадий и правила валидности балла сняты с эталона
 * дословно. Запись без обеих границ пропускается. skipped растёт на битых
 * записях и никогда не роняет тип целиком.
 */
std::vector<Domain::SleepSession> normalize_sleep(const std::vector<nlohmann::json>& records,
                                                  std::string_view user_id,
                                                  long& skipped);

/**
 * @brief Применить суточные отчёты к сессиям: одна однозначная главная сессия.
 *
 * Балл никогда не выдумывается и не размазывается: сегментные границы отчёта
 * авторитетны, без них выбор возможен только среди одного источника и
 * единственной самой длинной сессии, конфликт баллов оставляет NULL, своя
 * оценка записи не перетирается. default_zone_offset подставляется отчётам без
 * zone_offset (регион cn это 28800).
 */
void apply_daily_sleep_scores(std::vector<Domain::SleepSession>& sessions,
                              const std::vector<nlohmann::json>& reports,
                              int default_zone_offset);

/// Тренировки из записей отдельного эндпоинта. end_time при отсутствии это
/// start_time + duration. Все метрики через «ноль это NULL».
std::vector<Domain::Workout> normalize_workouts(const std::vector<nlohmann::json>& records,
                                                std::string_view user_id,
                                                long& skipped);

/// Вес и состав тела. Запись без веса пропускается целиком: это не измерение.
std::vector<Domain::BodyMeasurement> normalize_body(const std::vector<nlohmann::json>& records,
                                                    std::string_view user_id,
                                                    long& skipped);

/// Пульс: обычные записи (type 0 passive, иначе active) плюс отдельный ключ
/// покоя с sample_type resting и временем из date_time | time.
std::vector<Domain::HeartRateSample> normalize_heart_rate(const std::vector<nlohmann::json>& records,
                                                          const std::vector<nlohmann::json>& resting_records,
                                                          std::string_view user_id,
                                                          long& skipped);

/// SpO2 из spo2 | value; запись без значения пропускается.
std::vector<Domain::Spo2Sample> normalize_spo2(const std::vector<nlohmann::json>& records,
                                               std::string_view user_id,
                                               long& skipped);

/// Стресс из stress | score | value; уровень <30 low, <60 medium, иначе high.
std::vector<Domain::StressSample> normalize_stress(const std::vector<nlohmann::json>& records,
                                                   std::string_view user_id,
                                                   long& skipped);

/// Аномалии ритма; end_time при отсутствии равен start_time.
std::vector<Domain::AbnormalHeartBeatEvent> normalize_abnormal_heart_beat(const std::vector<nlohmann::json>& records,
                                                                          std::string_view user_id,
                                                                          long& skipped);

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
