/**
 * @file SyncService.hpp
 * @brief Оркестрация синка: куски диапазона, счётчики, журнал, изоляция типов.
 *
 * Один запуск на систему: переход queued -> running атомарен через частичный
 * уникальный индекс sync_runs, конкурент завершается статусом skipped, не
 * дойдя до облака. Провал одного типа не отменяет остальные: результат несёт
 * счётчики или класс ошибки (auth / protocol / timeout / other) по каждому.
 */

#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "xiaomi/Credentials.hpp"
#include "xiaomi/HttpTransport.hpp"

namespace Sync {

/// Канонический порядок типов, как у эталона.
inline const std::vector<std::string> kAllDataTypes = {
    "daily_activity", "heart_rate", "body_measurements", "sleep", "workouts", "spo2", "stress", "abnormal_heart_beat"};

class SyncService {
public:
    /// @param on_rotate сохранение ротированного токена; в тестах пустой.
    SyncService(Xiaomi::HttpTransport& transport,
                Xiaomi::Credentials credentials,
                std::function<void(const Xiaomi::Credentials&)> on_rotate = {});

    /**
     * @brief Выполнить запуск run_id за диапазон дат по списку типов.
     *
     * Сам переводит запуск в running, пишет результат и финальный статус в
     * sync_runs, обновляет sync_state по успешным типам. Возвращает результат
     * по типам. Наружу не бросает: любая судьба запуска отражена в журнале.
     */
    nlohmann::json run(long run_id,
                       const std::string& from,
                       const std::string& to,
                       const std::vector<std::string>& data_types);

private:
    Xiaomi::HttpTransport& transport_;
    Xiaomi::Credentials credentials_;
    std::function<void(const Xiaomi::Credentials&)> on_rotate_;
};

}  // namespace Sync
