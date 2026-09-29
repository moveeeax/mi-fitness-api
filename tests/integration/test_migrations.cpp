/**
 * @file test_migrations.cpp
 * @brief Integration tests for the MigrationRunner (needs real Postgres).
 *
 * Covers the behaviours that silently corrupt schema state if broken:
 * apply-pending, idempotent re-run (skip already-applied — the loser of a
 * concurrent boot relies on this), and the read-only list_pending tracker.
 * Uses a private temp migrations dir + a private tracking table-free schema
 * so it doesn't collide with the real 001_users_and_roles migration.
 */

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <gtest/gtest.h>

#include "database/Database.hpp"
#include "database/Migrations.hpp"
#include "test_helpers.hpp"
#include "utils/Crypto.hpp"

namespace fs = std::filesystem;

namespace {

class MigrationsTest : public ::testing::Test {
protected:
    fs::path dir_;

    void SetUp() override {
        if (!TestHelpers::is_postgres_available())
            GTEST_SKIP() << "PostgreSQL not available";
        TestHelpers::reset_all_globals();
        Database::initialize(TestHelpers::pg_conn_string(), {}, 2);

        // Clean slate: drop the test artifacts a previous run may have left.
        Database::get().execute_write([](auto& txn) {
            txn.exec("DROP TABLE IF EXISTS mig_test_widget");
            txn.exec("DROP TABLE IF EXISTS schema_migrations");
            return 0;
        });

        dir_ = fs::temp_directory_path() / "mig_test";
        fs::create_directories(dir_);
        std::ofstream(dir_ / "001_widget.sql")
            << "CREATE TABLE IF NOT EXISTS mig_test_widget (id serial primary key);\n";
    }

    void TearDown() override {
        if (Database::is_initialized()) {
            try {
                Database::get().execute_write([](auto& txn) {
                    txn.exec("DROP TABLE IF EXISTS mig_test_widget");
                    txn.exec("DROP TABLE IF EXISTS schema_migrations");
                    return 0;
                });
            } catch (...) {}
        }
        TestHelpers::reset_all_globals();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    long applied_count() {
        return Database::get().execute_read(
            [](auto& txn) { return txn.exec("SELECT COUNT(*) FROM schema_migrations")[0][0].template as<long>(); });
    }
};

TEST_F(MigrationsTest, AppliesPendingAndTracks) {
    EXPECT_EQ(Migrations::MigrationRunner::list_pending(dir_.string()).size(), 1u);

    Migrations::MigrationRunner runner;
    runner.initialize(dir_.string());

    EXPECT_EQ(applied_count(), 1);
    EXPECT_TRUE(Migrations::MigrationRunner::list_pending(dir_.string()).empty());
    // The migration actually ran.
    auto exists = Database::get().execute_read([](auto& txn) {
        return txn.exec("SELECT to_regclass('public.mig_test_widget') IS NOT NULL")[0][0].template as<bool>();
    });
    EXPECT_TRUE(exists);
}

TEST_F(MigrationsTest, ReRunIsIdempotent) {
    Migrations::MigrationRunner().initialize(dir_.string());
    ASSERT_EQ(applied_count(), 1);

    // A second runner over the same dir must skip the applied migration, not
    // re-run the DDL or duplicate the tracking row.
    Migrations::MigrationRunner runner2;
    EXPECT_NO_THROW(runner2.initialize(dir_.string()));
    EXPECT_EQ(applied_count(), 1);
}

TEST_F(MigrationsTest, NewMigrationAppliedOnNextRun) {
    Migrations::MigrationRunner().initialize(dir_.string());
    ASSERT_EQ(applied_count(), 1);

    std::ofstream(dir_ / "002_widget_col.sql") << "ALTER TABLE mig_test_widget ADD COLUMN IF NOT EXISTS label text;\n";
    EXPECT_EQ(Migrations::MigrationRunner::list_pending(dir_.string()).size(), 1u);

    Migrations::MigrationRunner().initialize(dir_.string());
    EXPECT_EQ(applied_count(), 2);
    EXPECT_TRUE(Migrations::MigrationRunner::list_pending(dir_.string()).empty());
}

TEST_F(MigrationsTest, RecordsChecksumOnApply) {
    Migrations::MigrationRunner().initialize(dir_.string());

    std::ifstream in(dir_ / "001_widget.sql");
    std::stringstream buf;
    buf << in.rdbuf();
    const std::string expected = Utils::Crypto::sha256_hex(buf.str());

    auto stored = Database::get().execute_read([](auto& txn) {
        return txn.exec("SELECT checksum FROM schema_migrations WHERE version = 1")[0][0].template as<std::string>();
    });
    EXPECT_EQ(stored, expected);
    EXPECT_EQ(stored.size(), 64u);
}

TEST_F(MigrationsTest, EditedAppliedMigrationThrows) {
    Migrations::MigrationRunner().initialize(dir_.string());

    // Rewrite the applied file — the exact "edit migration NNN in place"
    // mistake the checksum exists to catch (site 008_billing_refunds).
    std::ofstream(dir_ / "001_widget.sql")
        << "CREATE TABLE IF NOT EXISTS mig_test_widget (id serial primary key);\n-- sneaky edit\n";

    Migrations::MigrationRunner runner2;
    try {
        runner2.initialize(dir_.string());
        FAIL() << "editing an applied migration must throw";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("EDITED after being applied"), std::string::npos) << e.what();
    }
}

TEST_F(MigrationsTest, BackfillsChecksumForPreChecksumRows) {
    Migrations::MigrationRunner().initialize(dir_.string());
    // Simulate a row tracked before the checksum column existed.
    Database::get().execute_write([](auto& txn) {
        txn.exec("UPDATE schema_migrations SET checksum = NULL WHERE version = 1");
        return 0;
    });

    EXPECT_NO_THROW(Migrations::MigrationRunner().initialize(dir_.string()));
    auto null_count = Database::get().execute_read([](auto& txn) {
        return txn.exec("SELECT COUNT(*) FROM schema_migrations WHERE checksum IS NULL")[0][0].template as<long>();
    });
    EXPECT_EQ(null_count, 0);
}

}  // namespace
