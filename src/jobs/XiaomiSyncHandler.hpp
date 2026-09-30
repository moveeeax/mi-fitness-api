/**
 * @file XiaomiSyncHandler.hpp
 * @brief Обработчик задания xiaomi_sync: полный запуск SyncService в воркере.
 *
 * Payload: {run_id, from, to, data_types}. Учётные данные берутся из базы
 * ключом из конфигурации; их отсутствие это не авария очереди, а честный
 * статус not_configured в журнале запуска: ретрай без токена бессмыслен.
 */

#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "repositories/CredentialsRepository.hpp"
#include "repositories/SyncRunRepository.hpp"
#include "sync/SyncService.hpp"
#include "xiaomi/Service.hpp"

namespace Jobs::XiaomiSync {

inline constexpr const char* kJobType = "xiaomi_sync";

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
