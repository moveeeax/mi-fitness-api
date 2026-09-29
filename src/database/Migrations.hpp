/**
 * @file Migrations.hpp
 * @brief SQL migration runner module
 * @details Scans migrations directory for .sql files, tracks applied migrations
 *          in a schema_migrations table, and applies new ones on startup
 *
 * Non-template bodies (directory scan, checksum verification, the
 * transactional/no-transaction apply paths and the global-instance
 * lifecycle) live in Migrations.cpp (compiled once into app_core; ADR 0003
 * as amended 2026-08-22) — so database/Database.hpp is no longer exposed to
 * including TUs.
 */

#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Migrations {

namespace fs = std::filesystem;

struct MigrationFile {
    int version;
    std::string name;
    fs::path path;
};

/// A migration opts out of the wrapping transaction with a `-- migrate:no-transaction`
/// line, so it runs in autocommit with statement_timeout cleared — required for
/// CREATE INDEX CONCURRENTLY and long backfills, which cannot run inside a
/// transaction (and would be killed by the API statement_timeout). Such a file
/// MUST contain a SINGLE statement: libpq wraps a multi-statement string in an
/// implicit transaction, which CONCURRENTLY rejects.
bool has_no_transaction_marker(const std::string& sql);

/**
 * @brief Migration runner that applies SQL migrations on startup
 */
class MigrationRunner {
public:
    /**
     * @brief Scan a migrations directory and report which files haven't yet
     *        been applied to the database. Read-only — does not create the
     *        tracking table, does not apply anything. Caller must have
     *        initialized Database. Returns the sorted pending list by version.
     */
    static std::vector<MigrationFile> list_pending(const std::string& dir);

private:
    bool initialized = false;
    std::string migrations_dir;

    void ensure_tracking_table();

    std::vector<MigrationFile> scan_migrations();

    /// version → recorded checksum ("" for rows that predate the checksum
    /// column). Read-only SELECT — use a read txn on the primary (not a write
    /// slot), consistent with list_pending. Must be the primary: migrations
    /// are written there and a replica could lag.
    std::map<int, std::string> get_applied();

    std::string read_file(const fs::path& path);

    // Apply a `-- migrate:no-transaction` migration in AUTOCOMMIT with
    // statement_timeout cleared, so CREATE INDEX CONCURRENTLY / long backfills
    // work. A SESSION advisory lock (released on every path) serializes booting
    // replicas — the transaction-scoped lock the normal path uses needs a txn.
    // with_primary_connection restores the pool's statement_timeout afterwards.
    // @return true if applied, false if another instance applied it first.
    bool apply_no_transaction_(const MigrationFile& mf, const std::string& sql, const std::string& checksum);

public:
    void initialize(const std::string& dir);

    void shutdown();

    bool is_initialized() const { return initialized; }
};

/**
 * @brief Initialize the global migration runner instance
 */
void initialize(const std::string& migrations_dir);

MigrationRunner& get();

bool is_initialized();

void shutdown();

}  // namespace Migrations
