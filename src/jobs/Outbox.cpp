/**
 * @file Outbox.cpp
 * @brief Body for src/jobs/Outbox.hpp — compiled once into app_core (ADR 0003
 *        as amended 2026-08-22). The only jobs TU that includes the Database
 *        layer (edge declared in docs/module-deps.txt).
 */

#include "jobs/Outbox.hpp"

#include <exception>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "database/Database.hpp"
#include "jobs/Jobs.hpp"

namespace Jobs {
namespace Outbox {

namespace {

struct PendingRow {
    std::string id;
    std::string kind;
    std::string payload;  // jsonb as text; parsed at submit time
    int attempts = 0;
};

}  // namespace

DrainStats drain(int batch, int reclaim_sec) {
    DrainStats stats;
    if (batch <= 0)
        return stats;

    // 1. Claim a batch in a SHORT transaction — no Redis I/O while holding row
    //    locks. SKIP LOCKED makes concurrent drainers partition the backlog
    //    instead of blocking or double-claiming; the committed claimed_at then
    //    hides the rows from later passes until they're finalized (or the
    //    claim goes stale — reclaim_sec — because this process died here).
    auto rows = Database::get().execute_write([&](auto& txn) {
        std::vector<PendingRow> out;
        auto r = txn.exec_params(
            "UPDATE outbox SET claimed_at = now() "
            "WHERE id IN (SELECT id FROM outbox "
            "             WHERE claimed_at IS NULL "
            "                OR claimed_at < now() - make_interval(secs => $2::int) "
            "             ORDER BY created_at "
            "             LIMIT $1 "
            "             FOR UPDATE SKIP LOCKED) "
            "RETURNING id, kind, payload, attempts",
            batch,
            reclaim_sec);
        out.reserve(r.size());
        for (const auto& row : r) {
            out.push_back({row["id"].template as<std::string>(),
                           row["kind"].template as<std::string>(),
                           row["payload"].template as<std::string>(),
                           row["attempts"].template as<int>()});
        }
        return out;
    });
    if (rows.empty())
        return stats;

    // 2. Relay each row to the job queue. A per-row failure (Redis down, Jobs
    //    not initialized, unparseable payload) is recorded, never thrown —
    //    one poisoned row must not stall the rest of the batch.
    std::vector<std::string> submitted_ids;
    std::vector<std::pair<std::string, std::string>> failed;  // id -> error
    for (const auto& row : rows) {
        try {
            Jobs::get().submit(row.kind, json::parse(row.payload));
            submitted_ids.push_back(row.id);
        } catch (const std::exception& e) {
            spdlog::warn(
                "outbox: submit of {} (kind={}, attempt {}) failed: {}", row.id, row.kind, row.attempts + 1, e.what());
            failed.emplace_back(row.id, e.what());
        }
    }

    // 3. Finalize: drop the relayed rows, release the failed ones for the next
    //    pass. If THIS transaction is lost (process death, DB outage), the
    //    claims go stale and every row — including already-submitted ones — is
    //    redelivered after reclaim_sec: at-least-once, as documented.
    Database::get().execute_write([&](auto& txn) {
        for (const auto& id : submitted_ids)
            txn.exec_params("DELETE FROM outbox WHERE id = $1", id);
        for (const auto& [id, err] : failed)
            txn.exec_params(
                "UPDATE outbox SET claimed_at = NULL, attempts = attempts + 1, last_error = $2 WHERE id = $1", id, err);
        return 0;
    });

    stats.submitted = static_cast<long>(submitted_ids.size());
    stats.failed = static_cast<long>(failed.size());
    if (stats.submitted > 0 || stats.failed > 0)
        spdlog::debug("outbox: drained batch — {} submitted, {} failed", stats.submitted, stats.failed);
    return stats;
}

}  // namespace Outbox
}  // namespace Jobs
