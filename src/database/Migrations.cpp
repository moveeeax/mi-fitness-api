/**
 * @file Migrations.cpp
 * @brief Bodies for src/database/Migrations.hpp — compiled once into
 *        app_core: the directory scan, checksum verification/backfill, the
 *        transactional and no-transaction apply paths and the
 *        global-instance lifecycle. This is the only TU of the module that
 *        sees database/Database.hpp and pqxx; every contract is documented
 *        on the declarations in the header.
 */

#include "database/Migrations.hpp"

#include <algorithm>
#include <fstream>
#include <pqxx/pqxx>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <spdlog/spdlog.h>

#include "database/Database.hpp"
#include "utils/Crypto.hpp"

namespace Migrations {

bool has_no_transaction_marker(const std::string& sql) {
    return sql.find("-- migrate:no-transaction") != std::string::npos;
}

std::vector<MigrationFile> MigrationRunner::list_pending(const std::string& dir) {
    MigrationRunner scanner;
    scanner.migrations_dir = dir;
    auto on_disk = scanner.scan_migrations();
    std::set<int> applied;
    try {
        // Read from the primary: --verify-migrations must reflect what has
        // actually been applied, and migrations are written on the primary.
        // A lagging replica would report false-pending (or false-green).
        auto result = Database::get().execute_read_primary(
            [](auto& txn) { return txn.exec("SELECT version FROM schema_migrations ORDER BY version"); });
        for (const auto& row : result) {
            applied.insert(row[0].template as<int>());
        }
    } catch (const std::exception& e) {
        // No tracking table yet → every on-disk migration counts as pending.
        spdlog::debug("list_pending: schema_migrations not readable ({}), treating all as pending", e.what());
    }
    std::vector<MigrationFile> pending;
    for (auto& m : on_disk) {
        if (applied.count(m.version) == 0)
            pending.push_back(std::move(m));
    }
    return pending;
}

void MigrationRunner::ensure_tracking_table() {
    Database::get().execute_write([](auto& txn) {
        txn.exec(
            "CREATE TABLE IF NOT EXISTS schema_migrations ("
            "  version INTEGER PRIMARY KEY,"
            "  name VARCHAR(255) NOT NULL,"
            "  applied_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,"
            "  checksum VARCHAR(64)"
            ")");
        // Databases tracked before the checksum column existed get it
        // added in place; their rows stay NULL until backfilled on the
        // next boot's verify pass.
        txn.exec("ALTER TABLE schema_migrations ADD COLUMN IF NOT EXISTS checksum VARCHAR(64)");
        return 0;
    });
    spdlog::debug("schema_migrations table ensured");
}

std::vector<MigrationFile> MigrationRunner::scan_migrations() {
    std::vector<MigrationFile> migrations;
    std::regex pattern(R"(^(\d+)[_-].*\.sql$)");

    if (!fs::exists(migrations_dir) || !fs::is_directory(migrations_dir)) {
        spdlog::warn("Migrations directory '{}' does not exist, skipping", migrations_dir);
        return migrations;
    }

    for (const auto& entry : fs::directory_iterator(migrations_dir)) {
        if (!entry.is_regular_file())
            continue;

        std::string filename = entry.path().filename().string();
        std::smatch match;
        if (std::regex_match(filename, match, pattern)) {
            MigrationFile mf;
            mf.version = std::stoi(match[1].str());
            mf.name = filename;
            mf.path = entry.path();
            migrations.push_back(mf);
        }
    }

    std::sort(migrations.begin(), migrations.end(), [](const MigrationFile& a, const MigrationFile& b) {
        return a.version < b.version;
    });

    return migrations;
}

std::map<int, std::string> MigrationRunner::get_applied() {
    std::map<int, std::string> applied;
    auto result = Database::get().execute_read_primary(
        [](auto& txn) { return txn.exec("SELECT version, checksum FROM schema_migrations ORDER BY version"); });
    for (const auto& row : result) {
        applied[row[0].template as<int>()] = row[1].is_null() ? std::string{} : row[1].template as<std::string>();
    }
    return applied;
}

std::string MigrationRunner::read_file(const fs::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open migration file: " + path.string());
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool MigrationRunner::apply_no_transaction_(const MigrationFile& mf,
                                            const std::string& sql,
                                            const std::string& checksum) {
    return Database::get().with_primary_connection([&](pqxx::connection& c) -> bool {
        pqxx::nontransaction nt(c);
        nt.exec("SET statement_timeout = 0");
        nt.exec("SELECT pg_advisory_lock(4242424242)");
        bool applied = false;
        try {
            auto seen = nt.exec("SELECT 1 FROM schema_migrations WHERE version = $1", pqxx::params{mf.version});
            if (seen.empty()) {
                nt.exec(sql);  // single statement — autocommits immediately
                nt.exec("INSERT INTO schema_migrations (version, name, checksum) VALUES ($1, $2, $3)",
                        pqxx::params{mf.version, mf.name, checksum});
                applied = true;
            }
        } catch (...) {
            try {
                nt.exec("SELECT pg_advisory_unlock(4242424242)");
            } catch (...) {}
            throw;
        }
        nt.exec("SELECT pg_advisory_unlock(4242424242)");
        return applied;
    });
}

void MigrationRunner::initialize(const std::string& dir) {
    if (initialized) {
        throw std::runtime_error("Migration runner already initialized");
    }

    migrations_dir = dir;
    spdlog::info("Running database migrations from '{}'", migrations_dir);

    ensure_tracking_table();

    auto migrations = scan_migrations();
    if (migrations.empty()) {
        spdlog::info("No migration files found in '{}'", migrations_dir);
        initialized = true;
        return;
    }

    auto applied = get_applied();
    int applied_count = 0;
    int skipped_count = 0;

    for (const auto& mf : migrations) {
        const std::string sql = read_file(mf.path);
        const std::string checksum = Utils::Crypto::sha256_hex(sql);

        if (auto it = applied.find(mf.version); it != applied.end()) {
            // The runner keys on the VERSION NUMBER: a database that
            // recorded this version will never re-run the file, so an
            // in-place edit silently never reaches it (bit a downstream
            // fork on a billing migration — site 008_billing_refunds).
            // The checksum turns that silent divergence into a boot
            // failure.
            if (!it->second.empty() && it->second != checksum) {
                throw std::runtime_error("Migration " + mf.name + " (version " + std::to_string(mf.version) +
                                         ") was EDITED after being applied: recorded checksum " + it->second +
                                         " != on-disk sha256 " + checksum +
                                         ". Never edit an applied migration — ship a new NNN_*.sql with idempotent "
                                         "re-declarations instead; a database that already recorded this version "
                                         "will never re-run the edited file.");
            }
            if (it->second.empty()) {
                // Row predates the checksum column — backfill so the next
                // boot verifies this file too.
                Database::get().execute_write([&](auto& txn) {
                    txn.exec_params(
                        "UPDATE schema_migrations SET checksum = $1 WHERE version = $2 AND checksum IS NULL",
                        checksum,
                        mf.version);
                    return 0;
                });
            }
            skipped_count++;
            continue;
        }

        spdlog::info("Running migration {}: {}", mf.version, mf.name);

        // Hold a transaction-scoped advisory lock while applying so
        // concurrent boots (multiple replicas) serialize here instead of
        // both running the DDL and one crashing on the schema_migrations
        // PK conflict. Re-check applied-state INSIDE the lock: the loser
        // of the race finds the row already present and skips the DDL.
        bool did_apply;
        if (has_no_transaction_marker(sql)) {
            spdlog::info("Migration {} runs WITHOUT a transaction (autocommit, statement_timeout cleared)", mf.name);
            did_apply = apply_no_transaction_(mf, sql, checksum);
        } else
            did_apply = Database::get().execute_write([&](auto& txn) -> bool {
                txn.exec("SELECT pg_advisory_xact_lock(4242424242)");
                auto seen = txn.exec_params("SELECT 1 FROM schema_migrations WHERE version = $1", mf.version);
                if (!seen.empty())
                    return false;  // another booting instance applied it first
                txn.exec(sql);
                txn.exec_params("INSERT INTO schema_migrations (version, name, checksum) VALUES ($1, $2, $3)",
                                mf.version,
                                mf.name,
                                checksum);
                return true;
            });

        if (did_apply) {
            spdlog::info("Migration {} applied successfully", mf.name);
            applied_count++;
        } else {
            spdlog::info("Migration {} applied concurrently by another instance — skipped", mf.name);
            skipped_count++;
        }
    }

    spdlog::info("Migrations complete: {} applied, {} already up-to-date", applied_count, skipped_count);
    initialized = true;
}

void MigrationRunner::shutdown() {
    if (initialized) {
        spdlog::debug("Migration runner shut down");
        initialized = false;
    }
}

namespace {

/// Global migration runner instance. File-local on purpose: everything
/// outside this TU reaches it through initialize() / get() /
/// is_initialized() / shutdown() below (nothing else ever referenced it
/// directly when it was an inline header variable).
std::unique_ptr<MigrationRunner> global_runner = nullptr;

}  // namespace

void initialize(const std::string& migrations_dir) {
    if (global_runner != nullptr) {
        throw std::runtime_error("Migration runner already initialized");
    }
    global_runner = std::make_unique<MigrationRunner>();
    global_runner->initialize(migrations_dir);
}

MigrationRunner& get() {
    if (global_runner == nullptr) {
        throw std::runtime_error("Migration runner not initialized");
    }
    return *global_runner;
}

bool is_initialized() {
    return global_runner != nullptr && global_runner->is_initialized();
}

void shutdown() {
    if (global_runner) {
        global_runner->shutdown();
        global_runner.reset();
    }
}

}  // namespace Migrations
