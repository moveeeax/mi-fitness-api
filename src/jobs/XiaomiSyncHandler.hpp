/**
 * @file XiaomiSyncHandler.hpp
 * @brief Обработчик задания xiaomi_sync: полный запуск SyncService в воркере.
 *
 * Payload: {run_id, from, to, data_types}. Учётные данные берутся из базы
 * ключом из конфигурации; их отсутствие это не авария очереди, а честный
 * статус not_configured в журнале запуска: ретрай без токена бессмыслен.
 */

#pragma once

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "jobs/Jobs.hpp"
#include "repositories/CredentialsRepository.hpp"
#include "repositories/SyncRunRepository.hpp"
#include "sync/SyncService.hpp"
#include "utils/Config.hpp"
#include "xiaomi/Service.hpp"

namespace Jobs::XiaomiSync {

inline constexpr const char* kJobType = "xiaomi_sync";

/**
 * @brief Поставить в очередь синк последних @p window_days суток.
 *
 * Сутки считаются в поясе региона (cn это UTC+8, прочие UTC — то же
 * правило, что у границ диапазона в Xiaomi::range_to_timestamps): дата
 * «сегодня» по UTC около полуночи региона отстаёт на день, и окно
 * промахивалось бы мимо свежих данных. Возвращает run_id созданного
 * запуска; исключения базы и очереди отдаёт вызывающему.
 */
inline long enqueue_recent(int window_days, long long now_epoch) {
    if (window_days < 1) {
        window_days = 1;
    }
    std::string region;
    if (Config::is_initialized()) {
        region = Config::get().get<std::string>("xiaomi.region", "MI_FITNESS_REGION", "");
    }
    const long long offset = (region.empty() || region == "cn") ? 8 * 3600 : 0;

    const auto day = [](long long epoch) {
        const std::chrono::sys_days d{
            std::chrono::floor<std::chrono::days>(std::chrono::sys_seconds{std::chrono::seconds{epoch}})};
        const std::chrono::year_month_day ymd{d};
        char out[16];
        std::snprintf(out,
                      sizeof(out),
                      "%04d-%02u-%02u",
                      static_cast<int>(ymd.year()),
                      static_cast<unsigned>(ymd.month()),
                      static_cast<unsigned>(ymd.day()));
        return std::string(out);
    };
    const std::string to = day(now_epoch + offset);
    const std::string from = day(now_epoch + offset - 86400LL * (window_days - 1));

    Repositories::SyncRunRepository runs;
    const long run_id = runs.create(from, to, Sync::kAllDataTypes);
    Jobs::get().submit(
        kJobType, nlohmann::json{{"run_id", run_id}, {"from", from}, {"to", to}, {"data_types", Sync::kAllDataTypes}});
    return run_id;
}

inline nlohmann::json process_job(const nlohmann::json& payload) {
    const long run_id = payload.at("run_id").get<long>();
    const std::string from = payload.at("from").get<std::string>();
    const std::string to = payload.at("to").get<std::string>();
    std::vector<std::string> data_types = Sync::kAllDataTypes;
    if (payload.contains("data_types") && payload["data_types"].is_array() && !payload["data_types"].empty()) {
        data_types = payload["data_types"].get<std::vector<std::string>>();
    }

    Repositories::SyncRunRepository runs;
    const std::string token_key = Xiaomi::Service::token_key_b64();
    if (token_key.empty()) {
        runs.finish(run_id,
                    "failed",
                    nlohmann::json{{"error", "not_configured"}, {"detail", "MI_FITNESS_TOKEN_KEY is not set"}});
        return {{"run_id", run_id}, {"status", "failed"}};
    }
    Repositories::CredentialsRepository credentials_repo(token_key);
    const auto credentials = credentials_repo.load();
    if (!credentials.has_value()) {
        runs.finish(run_id,
                    "failed",
                    nlohmann::json{{"error", "not_configured"}, {"detail", "Xiaomi credentials are not seeded"}});
        return {{"run_id", run_id}, {"status", "failed"}};
    }

    Sync::SyncService service(
        Xiaomi::Service::transport(), *credentials, [&credentials_repo](const Xiaomi::Credentials& rotated) {
            credentials_repo.store(rotated);
        });
    const auto result = service.run(run_id, from, to, data_types);
    return {{"run_id", run_id}, {"result", result}};
}

}  // namespace Jobs::XiaomiSync
