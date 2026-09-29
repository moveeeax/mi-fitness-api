#include <gtest/gtest.h>

#include "database/Database.hpp"
#include "test_helpers.hpp"

static const std::string PG_CONN = TestHelpers::pg_conn_string();

// --- ConnectionPool tests ---

class ConnectionPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!TestHelpers::is_postgres_available()) {
            GTEST_SKIP() << "PostgreSQL not available";
        }
    }

    void TearDown() override { TestHelpers::reset_all_globals(); }
};

TEST_F(ConnectionPoolTest, Create) {
    Database::ConnectionPool pool(PG_CONN, 2);
    EXPECT_EQ(pool.size(), 2u);
    EXPECT_EQ(pool.active_count(), 0u);
    pool.shutdown();
}

TEST_F(ConnectionPoolTest, AcquireAndRelease) {
    Database::ConnectionPool pool(PG_CONN, 2);
    auto conn = pool.acquire();
    EXPECT_EQ(pool.active_count(), 1u);
    EXPECT_TRUE(conn->is_open());

    pool.release(std::move(conn));
    EXPECT_EQ(pool.active_count(), 0u);
    pool.shutdown();
}

// Regression: prior to lazy-fill, releasing a broken (null) connection would
// shrink the pool by one and never refill, eventually deadlocking acquire().
// With the fix, a fresh acquire() lazy-creates the replacement on demand.
TEST_F(ConnectionPoolTest, LazyFillAfterAllConnectionsBroken) {
    Database::ConnectionPool pool(PG_CONN, 2, std::chrono::seconds(2));

    auto c1 = pool.acquire();
    auto c2 = pool.acquire();
    EXPECT_EQ(pool.active_count(), 2u);

    // Simulate both connections broken in flight (dropped before release).
    pool.release(nullptr);
    pool.release(nullptr);
    EXPECT_EQ(pool.active_count(), 0u);

    // Old code: queue empty, predicate never satisfied → timeout.
    // New code: lazy-fill creates a fresh connection.
    auto c3 = pool.acquire();
    ASSERT_TRUE(c3 != nullptr);
    EXPECT_TRUE(c3->is_open());

    auto c4 = pool.acquire();
    ASSERT_TRUE(c4 != nullptr);
    EXPECT_TRUE(c4->is_open());

    pool.release(std::move(c3));
    pool.release(std::move(c4));
    pool.shutdown();
}

TEST_F(ConnectionPoolTest, PooledConnectionRAII) {
    Database::ConnectionPool pool(PG_CONN, 2);
    {
        Database::PooledConnection pc(pool);
        EXPECT_TRUE(pc.get() != nullptr);
        EXPECT_TRUE(pc->is_open());
        EXPECT_EQ(pool.active_count(), 1u);
    }
    // Connection returned after scope exit
    EXPECT_EQ(pool.active_count(), 0u);
    pool.shutdown();
}

// --- DatabaseManager tests ---

// Lifecycle tests drive initialize/shutdown themselves — no auto-init.
class DatabaseLifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!TestHelpers::is_postgres_available()) {
            GTEST_SKIP() << "PostgreSQL not available";
        }
    }

    void TearDown() override { TestHelpers::reset_all_globals(); }
};

// Operation tests get an initialized manager from the fixture.
class DatabaseManagerTest : public DatabaseLifecycleTest {
protected:
    void SetUp() override {
        DatabaseLifecycleTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Database::initialize(PG_CONN, {}, 2);
    }

    void TearDown() override {
        // Drop test table if it was created
        try {
            if (Database::is_initialized()) {
                Database::get().execute_write([](auto& txn) {
                    txn.exec("DROP TABLE IF EXISTS test_table");
                    return 0;
                });
            }
        } catch (...) {}
        TestHelpers::reset_all_globals();
    }
};

TEST_F(DatabaseLifecycleTest, InitializeAndShutdown) {
    Database::initialize(PG_CONN, {}, 2);
    EXPECT_TRUE(Database::is_initialized());

    Database::shutdown();
    EXPECT_FALSE(Database::is_initialized());
}

TEST_F(DatabaseLifecycleTest, DoubleInitThrows) {
    Database::initialize(PG_CONN, {}, 2);
    EXPECT_THROW(Database::initialize(PG_CONN, {}, 2), std::runtime_error);
}

TEST_F(DatabaseLifecycleTest, GetBeforeInitThrows) {
    EXPECT_THROW(Database::get(), std::runtime_error);
}

TEST_F(DatabaseManagerTest, HealthCheck) {
    EXPECT_TRUE(Database::get().health_check());
}

TEST_F(DatabaseManagerTest, ExecuteRead) {
    auto result = Database::get().execute_read([](auto& txn) { return txn.exec("SELECT 1 AS val"); });
    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0][0].template as<int>(), 1);
}

TEST_F(DatabaseManagerTest, ExecuteWrite) {
    Database::get().execute_write([](auto& txn) {
        txn.exec("CREATE TABLE IF NOT EXISTS test_table (id SERIAL PRIMARY KEY, name TEXT)");
        return 0;
    });

    Database::get().execute_write([](auto& txn) {
        txn.exec("INSERT INTO test_table (name) VALUES ('test_value')");
        return 0;
    });

    auto result = Database::get().execute_read(
        [](auto& txn) { return txn.exec("SELECT name FROM test_table WHERE name = 'test_value'"); });
    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0][0].template as<std::string>(), "test_value");
}

// --- execute_transaction / execute_write_idempotent (fork-facing API) ---
// Real-Postgres coverage for the two execute_* variants nothing in the
// template calls in production code — forks do (documented in
// docs/CONVENTIONS.md §3). The unit-level seam only proves they compile and
// enter the hook (test_database_seam.cpp); these prove the actual semantics.

TEST_F(DatabaseManagerTest, ExecuteTransactionRunsAtRequestedIsolationLevel) {
    auto isolation = [](auto& txn) {
        auto r = txn.exec("SHOW transaction_isolation");
        return r[0][0].template as<std::string>();
    };

    // Convenience overload = ReadCommitted (the Postgres default — no SET
    // TRANSACTION is issued on this path).
    EXPECT_EQ(Database::get().execute_transaction(isolation), "read committed");
    EXPECT_EQ(Database::get().execute_transaction(Database::IsolationLevel::ReadCommitted, isolation),
              "read committed");
    EXPECT_EQ(Database::get().execute_transaction(Database::IsolationLevel::RepeatableRead, isolation),
              "repeatable read");
    EXPECT_EQ(Database::get().execute_transaction(Database::IsolationLevel::Serializable, isolation), "serializable");
}

TEST_F(DatabaseManagerTest, ExecuteTransactionIsAtomicAcrossStatements) {
    Database::get().execute_write([](auto& txn) {
        txn.exec("DROP TABLE IF EXISTS test_table");
        txn.exec("CREATE TABLE test_table (id SERIAL PRIMARY KEY, name TEXT)");
        return 0;
    });

    // Happy path: both statements of one transaction land together.
    const int inserted = Database::get().execute_transaction([](auto& txn) {
        txn.exec("INSERT INTO test_table (name) VALUES ('a')");
        txn.exec("INSERT INTO test_table (name) VALUES ('b')");
        return 2;
    });
    EXPECT_EQ(inserted, 2);

    // Failure path: a throw after the first INSERT must roll BOTH back —
    // a non-pqxx exception is not classified transient, so no retry either.
    EXPECT_THROW(Database::get().execute_transaction([](auto& txn) {
        txn.exec("INSERT INTO test_table (name) VALUES ('c')");
        throw std::runtime_error("boom");
        return 0;
    }),
                 std::runtime_error);

    auto r = Database::get().execute_read([](auto& txn) { return txn.exec("SELECT COUNT(*) FROM test_table"); });
    EXPECT_EQ(r[0][0].template as<long>(), 2) << "the failed transaction must leave no partial rows";
}

TEST_F(DatabaseManagerTest, ExecuteWriteIdempotentUpsertConvergesOnReplay) {
    // execute_write_idempotent uses the liberal (read-style) retry classifier,
    // which may replay the whole transaction after a connection-class error a
    // commit could already have survived. That is safe ONLY for writes keyed
    // by a natural key — replaying converges instead of double-applying.
    // Exercise exactly that contract: an UPSERT keyed by name.
    Database::get().execute_write([](auto& txn) {
        txn.exec("DROP TABLE IF EXISTS test_table");
        txn.exec("CREATE TABLE test_table (name TEXT PRIMARY KEY, val INT NOT NULL)");
        return 0;
    });

    auto upsert = [](int val) {
        return Database::get().execute_write_idempotent([val](auto& txn) {
            txn.exec_params(
                "INSERT INTO test_table (name, val) VALUES ($1, $2) "
                "ON CONFLICT (name) DO UPDATE SET val = EXCLUDED.val",
                "job-42",
                val);
            return val;
        });
    };

    EXPECT_EQ(upsert(1), 1);
    EXPECT_EQ(upsert(2), 2);  // replay of the same logical write — converges

    auto r =
        Database::get().execute_read([](auto& txn) { return txn.exec("SELECT COUNT(*), MAX(val) FROM test_table"); });
    EXPECT_EQ(r[0][0].template as<long>(), 1) << "same key must never duplicate";
    EXPECT_EQ(r[0][1].template as<int>(), 2) << "last write wins";
}

TEST_F(DatabaseLifecycleTest, UnreachableReplicaDoesNotBlockBoot) {
    // Replica is an optional read optimization: a dead replica URL must not
    // fail initialization (it used to crash-loop the whole app). Reads then
    // serve from the primary.
    Database::initialize(PG_CONN, {"postgresql://postgres:postgres@no-such-replica-host:5432/appdb"}, 2);
    EXPECT_TRUE(Database::is_initialized());
    auto result = Database::get().execute_read([](auto& txn) { return txn.exec("SELECT 1 AS one"); });
    EXPECT_EQ(result[0][0].template as<int>(), 1);
}

TEST_F(DatabaseManagerTest, ReplicaFallbackToPrimary) {
    // Fixture initializes without replicas — get_replica() must fall back
    // to the primary pool.
    auto conn = Database::get().get_replica();
    EXPECT_TRUE(conn.get() != nullptr);
    EXPECT_TRUE(conn->is_open());
}

TEST_F(DatabaseLifecycleTest, ShutdownIdempotent) {
    Database::initialize(PG_CONN, {}, 2);
    EXPECT_NO_THROW(Database::shutdown());
    EXPECT_NO_THROW(Database::shutdown());
}
