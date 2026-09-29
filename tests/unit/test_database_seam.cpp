/**
 * @file test_database_seam.cpp
 * @brief Proves the Database DI/fake seam: the database singleton can be
 *        swapped for a counting fake via Database::install_for_testing, and
 *        the whole execute_* template family runs against it WITHOUT a live
 *        Postgres — no pool, no pqxx::connection, no retry. Mirror of the
 *        cache seam demonstrated in test_di_fake.cpp. Pure unit.
 *
 *        Scope of the seam (see the ErasedTxn note in Database.hpp): fakes
 *        observe statement templates + call counts and return EMPTY
 *        pqxx::results; faking row data needs the Phase 2 de-inline of the
 *        row/result layer.
 */

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "database/Database.hpp"

namespace {

/// Statement-level fake: counts calls and records query templates. Returns
/// default-constructed (empty) pqxx::results — constructing a populated
/// result requires a server, which is exactly what this test avoids.
class CountingTxn : public Database::detail::ErasedTxn {
public:
    int exec_params_calls = 0;
    int exec_calls = 0;
    std::vector<std::string> queries;

    pqxx::result exec_params_erased(const std::string& query, const pqxx::params& /*params*/) override {
        ++exec_params_calls;
        queries.push_back(query);
        return pqxx::result{};
    }

    pqxx::result exec_erased(const std::string& query) override {
        ++exec_calls;
        queries.push_back(query);
        return pqxx::result{};
    }
};

/// DatabaseManager fake — never initialized, never connects. Overrides the
/// three seam points: liveness (is_initialized/health_check) and the
/// per-transaction hook, counting each entry as "<op>@<pool>".
class FakeDatabase : public Database::DatabaseManager {
public:
    CountingTxn txn;
    std::map<std::string, int> entered;  // e.g. "db.write@primary" -> 2

    bool is_initialized() const override { return true; }
    bool health_check() override { return true; }

protected:
    Database::detail::ErasedTxn* test_transaction_(const char* op, const char* pool) override {
        ++entered[std::string(op) + "@" + pool];
        return &txn;
    }
};

class DatabaseSeamTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto fake = std::make_unique<FakeDatabase>();
        fake_ = fake.get();
        Database::install_for_testing(std::move(fake));
    }
    void TearDown() override { Database::reset_for_testing(); }

    FakeDatabase* fake_ = nullptr;
};

TEST_F(DatabaseSeamTest, FakeInstallsAndResets) {
    EXPECT_TRUE(Database::is_initialized());
    EXPECT_TRUE(Database::get().health_check());

    Database::reset_for_testing();
    EXPECT_FALSE(Database::is_initialized());
    EXPECT_THROW(Database::get(), std::runtime_error);
}

TEST_F(DatabaseSeamTest, ExecuteWriteRunsAgainstFakeAndCounts) {
    const int rc = Database::get().execute_write([](auto& txn) {
        txn.exec_params("INSERT INTO t(a) VALUES ($1)", 42);
        txn.exec_params("UPDATE t SET a = $1 WHERE id = $2", 7, 1);
        return 123;  // the body's return value flows through untouched
    });

    EXPECT_EQ(rc, 123);
    EXPECT_EQ(fake_->entered["db.write@primary"], 1);
    EXPECT_EQ(fake_->txn.exec_params_calls, 2);
    ASSERT_EQ(fake_->txn.queries.size(), 2u);
    // The fake sees the QUERY TEMPLATE (placeholders), not parameter values.
    EXPECT_EQ(fake_->txn.queries[0], "INSERT INTO t(a) VALUES ($1)");
}

TEST_F(DatabaseSeamTest, ReadRoutingIntentIsObservable) {
    auto count_rows = [](auto& txn) {
        auto r = txn.exec("SELECT 1");
        return r.size();  // empty fake result — 0 rows, but fully usable
    };

    EXPECT_EQ(Database::get().execute_read(count_rows), 0);
    EXPECT_EQ(Database::get().execute_read_primary(count_rows), 0);

    EXPECT_EQ(fake_->entered["db.read@replica"], 1);  // execute_read's intent
    EXPECT_EQ(fake_->entered["db.read@primary"], 1);  // read-after-write path
    EXPECT_EQ(fake_->txn.exec_calls, 2);
}

TEST_F(DatabaseSeamTest, EveryExecuteVariantEntersTheHookOnce) {
    auto noop = [](auto& txn) {
        txn.exec("SELECT 1");
        return 0;
    };

    Database::get().execute_write(noop);
    Database::get().execute_write_idempotent(noop);
    Database::get().execute_transaction(noop);
    Database::get().execute_transaction(Database::IsolationLevel::Serializable, noop);

    EXPECT_EQ(fake_->entered["db.write@primary"], 1);
    EXPECT_EQ(fake_->entered["db.write.idempotent@primary"], 1);
    EXPECT_EQ(fake_->entered["db.transaction@primary"], 2);
    EXPECT_EQ(fake_->txn.exec_calls, 4);
}

TEST_F(DatabaseSeamTest, RawEscapeHatchThrowsOnFake) {
    // raw() hands out the underlying pqxx transaction — a fake has none, and
    // the seam must say so loudly rather than hand back a dangling reference.
    Database::get().execute_write([](auto& txn) {
        EXPECT_THROW(txn.raw(), std::logic_error);
        return 0;
    });
}

}  // namespace
