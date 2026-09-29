/**
 * @file test_outbox.cpp
 * @brief Integration tests for the transactional outbox (src/jobs/Outbox.hpp
 *        + migration 010): enqueue rides the caller's Postgres transaction,
 *        drain relays committed rows to the Redis job queue.
 *
 * The suite pins the four properties that ARE the pattern:
 *   1. commit → drain submits the job (payload intact);
 *   2. rollback → the event never existed (atomicity with the domain write);
 *   3. a failed submit is recorded (attempts/last_error), released, and a
 *      later drain retries it to success;
 *   4. concurrent drains partition the backlog (SKIP LOCKED) — every row
 *      submitted exactly once, none lost.
 *
 * Requires live Postgres (migrations on — 010 creates the table) and Redis.
 */

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "database/Database.hpp"
#include "jobs/Jobs.hpp"
#include "jobs/Outbox.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;

namespace {

class OutboxTest : public TestHelpers::CoreBackedTest {
protected:
    // Outbox kinds double as job-queue names; TearDown drains exactly these.
    static constexpr const char* kKinds[] = {"outboxq", "outboxq_rb", "outboxq_fail", "outboxq_conc"};

    std::string config_file_name() const override { return "outbox_test_config.json"; }

    void config_overrides(json& cfg) override {
        cfg["logging"]["name"] = "outbox_test";
        cfg["logging"]["file"] = "logs/outbox_test.log";
        cfg["observability"]["service_name"] = "outbox_svc";
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
        cfg["jobs"]["enabled"] = true;
        cfg["jobs"]["result_ttl"] = 3600;
        cfg["jobs"]["max_retries"] = 2;
        // Deliberately NOT setting outbox.drain_interval_sec: the default 0
        // must schedule nothing (opt-in contract) — every drain below is an
        // explicit call, so a stray background tick would break the counts.
    }

    // pick() needs a blocking client whose socket_timeout outlives the BRPOP
    // argument (same rationale as test_jobs.cpp).
    void post_init() override {
        Jobs::get().init_blocking_client(
            TestHelpers::redis_host(), TestHelpers::redis_port(), /*brpop_timeout_sec=*/5, /*password=*/"");
    }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        truncate_outbox();
    }

    void TearDown() override {
        if (Database::is_initialized()) {
            try {
                truncate_outbox();
            } catch (...) {}
        }
        if (Cache::is_initialized())
            TestHelpers::drain_jobs({std::begin(kKinds), std::end(kKinds)});
        TestHelpers::CoreBackedTest::TearDown();
    }

    static void truncate_outbox() {
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE outbox");
            return 0;
        });
    }

    static long outbox_count() {
        return Database::get().execute_read_primary(
            [](auto& txn) { return txn.exec("SELECT count(*) FROM outbox")[0][0].template as<long>(); });
    }
};

TEST_F(OutboxTest, EnqueueCommitDrainSubmitsJob) {
    Database::get().execute_transaction([](auto& txn) {
        Jobs::Outbox::enqueue(txn, "outboxq", {{"n", 1}, {"who", "outbox"}});
        return 0;
    });
    ASSERT_EQ(outbox_count(), 1);

    auto stats = Jobs::Outbox::drain();
    EXPECT_EQ(stats.submitted, 1);
    EXPECT_EQ(stats.failed, 0);
    // The relayed row is gone — a second drain must find nothing (no dup).
    EXPECT_EQ(outbox_count(), 0);
    auto again = Jobs::Outbox::drain();
    EXPECT_EQ(again.submitted, 0);

    // The job actually reached the Redis queue, payload intact.
    auto picked = Jobs::get().pick({"outboxq"}, 2, "w1");
    ASSERT_TRUE(picked);
    EXPECT_EQ(picked->type, "outboxq");
    EXPECT_EQ(picked->payload.at("n").get<int>(), 1);
    EXPECT_EQ(picked->payload.at("who").get<std::string>(), "outbox");
    Jobs::get().complete(picked->id, {{"ok", true}});
}

TEST_F(OutboxTest, RolledBackEnqueueIsInvisible) {
    // The atomicity that IS the pattern: the domain write and the event share
    // one transaction, so a rollback erases both.
    EXPECT_THROW(Database::get().execute_transaction([](auto& txn) -> int {
        Jobs::Outbox::enqueue(txn, "outboxq_rb", {{"n", 2}});
        throw std::runtime_error("simulated domain-write failure after enqueue");
    }),
                 std::runtime_error);

    EXPECT_EQ(outbox_count(), 0);
    auto stats = Jobs::Outbox::drain();
    EXPECT_EQ(stats.submitted, 0);
    EXPECT_EQ(stats.failed, 0);

    // Nothing reached Redis either.
    const auto depth = Jobs::get().queue_depth_by_type();
    EXPECT_EQ(depth.count("outboxq_rb") ? depth.at("outboxq_rb") : 0, 0);
}

TEST_F(OutboxTest, FailedSubmitBumpsAttemptsAndLaterDrainDelivers) {
    Database::get().execute_transaction([](auto& txn) {
        Jobs::Outbox::enqueue(txn, "outboxq_fail", {{"n", 3}});
        return 0;
    });

    // Take the job queue down: submit now throws, which must be RECORDED on
    // the row — never lost, never thrown out of drain.
    Jobs::shutdown();
    auto s1 = Jobs::Outbox::drain();
    EXPECT_EQ(s1.submitted, 0);
    EXPECT_EQ(s1.failed, 1);

    struct RowState {
        int attempts;
        std::string last_error;
        bool released;
    };
    auto state1 = Database::get().execute_read_primary([](auto& txn) {
        auto r = txn.exec("SELECT attempts, last_error, claimed_at IS NULL AS released FROM outbox");
        return RowState{r[0]["attempts"].template as<int>(),
                        r[0]["last_error"].template as<std::string>(),
                        r[0]["released"].template as<bool>()};
    });
    EXPECT_EQ(state1.attempts, 1);
    EXPECT_FALSE(state1.last_error.empty());
    EXPECT_TRUE(state1.released) << "a failed row must be released (claimed_at NULL) so the next drain retries it";

    // Still down: the retry happens immediately on the next drain and keeps
    // counting.
    auto s2 = Jobs::Outbox::drain();
    EXPECT_EQ(s2.failed, 1);
    auto attempts2 = Database::get().execute_read_primary(
        [](auto& txn) { return txn.exec("SELECT attempts FROM outbox")[0][0].template as<int>(); });
    EXPECT_EQ(attempts2, 2);

    // Queue back up → the event finally goes out. Crash-shaped outages heal
    // the same way; nothing was lost meanwhile.
    Jobs::initialize(/*result_ttl=*/3600, /*max_retries=*/2);
    Jobs::get().init_blocking_client(
        TestHelpers::redis_host(), TestHelpers::redis_port(), /*brpop_timeout_sec=*/5, /*password=*/"");
    auto s3 = Jobs::Outbox::drain();
    EXPECT_EQ(s3.submitted, 1);
    EXPECT_EQ(s3.failed, 0);
    EXPECT_EQ(outbox_count(), 0);

    auto picked = Jobs::get().pick({"outboxq_fail"}, 2, "w1");
    ASSERT_TRUE(picked);
    EXPECT_EQ(picked->payload.at("n").get<int>(), 3);
    Jobs::get().complete(picked->id, {{"ok", true}});
}

TEST_F(OutboxTest, ConcurrentDrainsSubmitEveryRowExactlyOnce) {
    constexpr int kRows = 40;
    Database::get().execute_transaction([](auto& txn) {
        for (int i = 0; i < kRows; ++i)
            Jobs::Outbox::enqueue(txn, "outboxq_conc", {{"n", i}});
        return 0;
    });
    ASSERT_EQ(outbox_count(), kRows);

    const auto before = Jobs::get().queue_depth_by_type();
    const long base = before.count("outboxq_conc") ? before.at("outboxq_conc") : 0;

    // Two drainers race over the same backlog in small batches. SKIP LOCKED
    // must make them partition it: no row claimed twice, none skipped forever.
    std::atomic<long> submitted{0};
    auto drainer = [&] {
        for (int pass = 0; pass < 10; ++pass) {
            auto stats = Jobs::Outbox::drain(/*batch=*/5);
            submitted += stats.submitted;
            EXPECT_EQ(stats.failed, 0);
        }
    };
    std::thread t1(drainer), t2(drainer);
    t1.join();
    t2.join();

    EXPECT_EQ(submitted.load(), kRows);
    EXPECT_EQ(outbox_count(), 0);

    // Redis agrees: exactly kRows jobs queued — a duplicate claim would
    // overshoot, a lost row would undershoot.
    const auto after = Jobs::get().queue_depth_by_type();
    ASSERT_TRUE(after.count("outboxq_conc"));
    EXPECT_EQ(after.at("outboxq_conc"), base + kRows);
}

}  // namespace
