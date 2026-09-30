/**
 * @file SyncRunRepository.hpp
 * @brief Журнал запусков синка (миграция 012, таблица sync_runs).
 *
 * Запуск создаётся до постановки задания в очередь и закрывается результатом
 * по типам данных. Статусы: running, succeeded, failed, interrupted, skipped.
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "database/Database.hpp"

namespace Repositories {

/// Ключ advisory lock единственного одновременного синка. Произвольная
/// константа, важно лишь то, что она одна на систему.
inline constexpr long long kSyncAdvisoryLockKey = 0x4d69466974;

class SyncRunRepository {
public:
    long create(const std::string& from, const std::string& to, const std::vector<std::string>& data_types) {
        // Типы это внутренние идентификаторы без запятых и скобок, поэтому
        // литерал массива собирается конкатенацией без экранирования.
        std::string array_literal = "{";
        for (const auto& type : data_types) {
            if (array_literal.size() > 1) {
                array_literal += ",";
            }
            array_literal += type;
        }
        array_literal += "}";
        return Database::get().execute_write([&](auto& txn) {
            // Статус queued, не running: строка running одна на систему по
            // частичному уникальному индексу, и занимать её должен исполнитель
            // атомарным переходом, а не постановщик.
            auto r = txn.exec_params(
                "INSERT INTO sync_runs (status, requested_start, requested_end, data_types) "
                "VALUES ('queued', $1, $2, $3::text[]) RETURNING id",
                from,
                to,
                array_literal);
            return r[0][0].template as<long>();
        });
    }

    void finish(long id, const std::string& status, const nlohmann::json& result) {
        Database::get().execute_write([&](auto& txn) {
            txn.exec_params(
                "UPDATE sync_runs SET status = $2, result = $3::jsonb, finished_at = now() "
                "WHERE id = $1",
                id,
                status,
                result.dump());
            return true;
        });
    }

    /// Пометить skipped только запуск, всё ещё стоящий в очереди. Повторно
    /// доставленное задание завершённого запуска не должно переписывать его
    /// журнал (Important 4 финального обзора: та же дыра со стороны skip).
    void skip_if_queued(long id, const nlohmann::json& result) {
        Database::get().execute_write([&](auto& txn) {
            txn.exec_params(
                "UPDATE sync_runs SET status = 'skipped', result = $2::jsonb, finished_at = now() "
                "WHERE id = $1 AND status = 'queued'",
                id,
                result.dump());
            return true;
        });
    }

    std::optional<nlohmann::json> get(long id) {
        return Database::get().execute_read([&](auto& txn) -> std::optional<nlohmann::json> {
            auto r = txn.exec_params(
                "SELECT id, started_at::text, finished_at::text, status, "
                "requested_start::text, requested_end::text, "
                "array_to_json(data_types)::text, result::text "
                "FROM sync_runs WHERE id = $1",
                id);
            if (r.empty()) {
                return std::nullopt;
            }
            const auto& row = r[0];
            nlohmann::json out;
            out["id"] = row[0].template as<long>();
            out["started_at"] = row[1].template as<std::string>();
            out["finished_at"] =
                row[2].is_null() ? nlohmann::json() : nlohmann::json(row[2].template as<std::string>());
            out["status"] = row[3].template as<std::string>();
            out["requested_start"] =
                row[4].is_null() ? nlohmann::json() : nlohmann::json(row[4].template as<std::string>());
            out["requested_end"] =
                row[5].is_null() ? nlohmann::json() : nlohmann::json(row[5].template as<std::string>());
            out["data_types"] = nlohmann::json::parse(row[6].template as<std::string>());
            out["result"] = nlohmann::json::parse(row[7].template as<std::string>());
            return out;
        });
    }
};

}  // namespace Repositories
