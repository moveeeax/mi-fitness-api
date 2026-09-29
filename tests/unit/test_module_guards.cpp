/**
 * @file test_module_guards.cpp
 * @brief Unit tests for the lifecycle/guard contracts of modules that have
 *        no other direct coverage: Messaging (incl. the MessagingSystem /
 *        producer / consumer pre-init guards), Tasks, Migrations' singleton
 *        doorway, the JobQueue config accessors, and the SqlErrors
 *        translate_sql wrapper. Pure — no Kafka broker, no Postgres/Redis.
 */

#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "database/Migrations.hpp"
#include "jobs/Jobs.hpp"
#include "messaging/Messaging.hpp"
#include "repositories/SqlErrors.hpp"
#include "tasks/Tasks.hpp"

namespace {

// ---- Messaging lifecycle (no broker needed: ctor doesn't connect) ----------

TEST(MessagingGuardTest, GetBeforeInitThrows) {
    if (Messaging::is_initialized())
        Messaging::shutdown();
    EXPECT_FALSE(Messaging::is_initialized());
    EXPECT_THROW(Messaging::get(), std::runtime_error);
}

TEST(MessagingGuardTest, InitThrowsOnDoubleInitAndShutdownResets) {
    if (Messaging::is_initialized())
        Messaging::shutdown();
    Messaging::initialize();
    EXPECT_TRUE(Messaging::is_initialized());
    EXPECT_NO_THROW(Messaging::get());
    // Messaging follows the throw-on-reinit convention (like Cache/Jobs/
    // Database), NOT the warned-no-op one (Auth/RateLimit/Idempotency).
    EXPECT_THROW(Messaging::initialize(), std::runtime_error);
    EXPECT_TRUE(Messaging::is_initialized());
    Messaging::shutdown();
    EXPECT_FALSE(Messaging::is_initialized());
    EXPECT_THROW(Messaging::get(), std::runtime_error);
}

// Smoke over the template-API surface below Messaging::get() — the accessor
// guards that forks hit first when wiring a producer/consumer. None of this
// touches librdkafka objects: everything must throw/report BEFORE any broker
// I/O could happen.
TEST(MessagingGuardTest, SystemAccessorsGuardBeforeComponentInit) {
    if (Messaging::is_initialized())
        Messaging::shutdown();
    Messaging::initialize();
    auto& sys = Messaging::get();
    EXPECT_FALSE(sys.has_producer());
    EXPECT_FALSE(sys.has_consumer());
    EXPECT_THROW(sys.get_producer(), std::runtime_error);
    EXPECT_THROW(sys.get_consumer(), std::runtime_error);
    Messaging::shutdown();
}

TEST(MessagingGuardTest, ProducerAndConsumerOpsThrowBeforeInit) {
    Messaging::KafkaProducer producer;
    EXPECT_FALSE(producer.is_initialized());
    EXPECT_THROW(producer.produce("topic", "key", "payload"), std::runtime_error);
    EXPECT_THROW(producer.flush(0), std::runtime_error);
    EXPECT_THROW(producer.outq_len(), std::runtime_error);

    Messaging::KafkaConsumer consumer;
    EXPECT_FALSE(consumer.is_initialized());
    EXPECT_FALSE(consumer.is_consuming());
    EXPECT_THROW(consumer.consume(0), std::runtime_error);
    EXPECT_THROW(consumer.start_consuming([](const std::string&, const std::string&) {}, 0), std::runtime_error);
    // stop_consuming/shutdown are deliberately safe no-ops pre-init.
    EXPECT_NO_THROW(consumer.stop_consuming());
    EXPECT_NO_THROW(consumer.shutdown());
    EXPECT_NO_THROW(producer.shutdown());
}

// ---- Migrations singleton doorway ------------------------------------------

TEST(MigrationsGuardTest, GetBeforeInitThrows) {
    if (Migrations::is_initialized())
        Migrations::shutdown();
    EXPECT_FALSE(Migrations::is_initialized());
    EXPECT_THROW(Migrations::get(), std::runtime_error);
}

// ---- JobQueue config accessors ---------------------------------------------

TEST(JobsAccessorTest, DefaultsVisibleThroughAccessors) {
    // A fresh (never-initialized) JobQueue exposes the documented defaults —
    // the same values Jobs::initialize() falls back to (JOBS_MAX_RETRIES=3,
    // JOBS_RESULT_TTL=86400 in docs/CONFIG.md). No Redis involved.
    Jobs::JobQueue q;
    EXPECT_FALSE(q.is_initialized());
    EXPECT_EQ(q.default_max_retries(), 3);
    EXPECT_EQ(q.result_ttl(), 86400);
}

// ---- Tasks guards ----------------------------------------------------------

TEST(TasksGuardTest, ScheduleBeforeInitThrows) {
    if (Tasks::is_initialized())
        Tasks::shutdown();
    EXPECT_THROW(Tasks::schedule_recurring("t", std::chrono::milliseconds(1000), [] {}), std::runtime_error);
}

TEST(TasksGuardTest, CancelUnknownReturnsFalse) {
    // cancel() doesn't require init and must not touch the event loop for a
    // miss — safe to call in a unit test.
    EXPECT_FALSE(Tasks::cancel("does-not-exist"));
}

// ---- SqlErrors::translate_sql ----------------------------------------------

TEST(SqlErrorsTest, ReturnsBodyResultWhenNoError) {
    int calls = 0;
    auto translator = [&](std::string_view) { ++calls; };  // must NOT be called
    int r = Repositories::detail::translate_sql([] { return 42; }, translator);
    EXPECT_EQ(r, 42);
    EXPECT_EQ(calls, 0);
}

TEST(SqlErrorsTest, NonSqlErrorPropagatesUnchangedAndSkipsTranslator) {
    int calls = 0;
    auto translator = [&](std::string_view) { ++calls; };
    EXPECT_THROW(Repositories::detail::translate_sql(
                     [] {
                         throw std::runtime_error("not a sql_error");
                         return 0;
                     },
                     translator),
                 std::runtime_error);
    EXPECT_EQ(calls, 0) << "translator must only fire on pqxx::sql_error";
}

}  // namespace
