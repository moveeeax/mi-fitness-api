/**
 * @file Jobs.hpp
 * @brief Redis-backed job queue module
 * @details Provides job submission, retrieval, and lifecycle management
 *          using Redis lists (LPUSH/BRPOP) for distributed work queues.
 *
 * Declarations only — the bodies live in Jobs.cpp (compiled once into
 * app_core; ADR 0003 as amended 2026-08-22): including this header no
 * longer pulls redis++/spdlog/Cache/Trace into the including TU. The
 * nlohmann json alias arrives via jobs/Job.hpp.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "jobs/Job.hpp"

namespace sw {
namespace redis {
class Redis;
}  // namespace redis
}  // namespace sw

namespace Jobs {

/**
 * @brief Job queue manager backed by Redis
 *
 * Exception contract (for callers adding new endpoints):
 *   - submit / pick / complete / fail / cancel / get_status / list_paged /
 *     list_dlq / requeue_from_dlq THROW on Redis errors or missing state —
 *     controllers map them to 5xx.
 *   - dlq_depth / dlq_depth_by_type / health_check / set_trace_id and the
 *     index ZADDs inside submit are best-effort: they swallow Redis errors
 *     (observability must not take the queue down with it).
 */
class JobQueue {
private:
    bool initialized_ = false;
    long result_ttl_ = 86400;  // seconds
    int default_max_retries_ = 3;
    // Retry backoff (opt-in). 0 = legacy behaviour: fail() requeues immediately.
    // >0 = a failed job is parked in jobs:delayed for an exponentially growing
    // delay (base * 2^(retry_count-1), capped at max), promoted back by
    // promote_due_jobs() — kills the tight pick→fail→pick loop on a near-empty
    // queue.
    int64_t retry_backoff_base_ms_ = 0;
    int64_t retry_backoff_max_ms_ = 60000;
    // Visibility timeout (opt-in). 0 = leases disabled (only same-worker startup
    // recovery). >0 = pick() leases the job in jobs:leases for this many ms;
    // reap_expired_leases() reclaims any lease a crashed worker never cleared,
    // re-running it via fail() (so a permanently-stuck job eventually DLQs).
    int64_t visibility_timeout_ms_ = 0;
    // shared_ptr (not unique_ptr) so pick() can hold a local copy for the
    // duration of its BRPOP: shutdown() dropping the manager's reference then
    // can't free the client out from under an in-flight blocking pop.
    // sw::redis::Redis is forward-declared above — construction/destruction
    // sites live in Jobs.cpp, and shared_ptr's type-erased deleter keeps the
    // implicit ~JobQueue() valid against the incomplete type.
    std::shared_ptr<sw::redis::Redis> blocking_client_;  // for BRPOP (long socket_timeout)

public:
    void initialize(long result_ttl = 86400, int max_retries = 3);

    /**
     * @brief Enable exponential retry backoff. base_ms=0 disables it (legacy
     *        immediate requeue). Called once during init from config.
     */
    void set_retry_backoff(int64_t base_ms, int64_t max_ms);

    /**
     * @brief Enable the visibility-timeout lease. sec<=0 disables it. Called
     *        once during init from config.
     */
    void set_visibility_timeout(int64_t sec) { visibility_timeout_ms_ = sec <= 0 ? 0 : sec * 1000; }

    /**
     * @brief Dedicated standalone Redis client for blocking BRPOP. The Cache
     *        pool has too tight a socket_timeout for blocking ops; this one
     *        uses (brpop_timeout + 2s) so the socket doesn't trip first.
     */
    void init_blocking_client(const std::string& host,
                              int port,
                              long brpop_timeout_sec,
                              const std::string& password = "",
                              int pool_size = 4,
                              int db = 0);

    /**
     * @brief Dedicated Sentinel-aware Redis client for blocking BRPOP.
     */
    void init_blocking_client_sentinel(const std::string& master_name,
                                       const std::vector<std::pair<std::string, int>>& sentinels,
                                       long brpop_timeout_sec,
                                       const std::string& password = "",
                                       const std::string& sentinel_password = "",
                                       int pool_size = 4,
                                       int db = 0);

    /**
     * @brief Submit a new job to the queue
     */
    Job submit(const std::string& type, const json& payload, int max_retries = -1);

    /**
     * @brief Pick the next job from one or more queues (blocking pop).
     * @details Uses BRPOPLPUSH (or multi-key BRPOP + RPUSH fallback) to move
     *          the id atomically from the live queue into the worker's
     *          per-worker processing list, so a worker crashing between
     *          "got id" and "set status=processing" doesn't lose the job:
     *          recover_processing() on startup pushes leftovers back.
     *          Only single-type picks get true atomicity; multi-type picks
     *          fall back to BRPOP + explicit RPUSH to processing list
     *          (a narrow window remains but shrinks from "full processing"
     *          to a single command — acceptable trade-off).
     * @param types Queue types to pop from
     * @param timeout_sec BRPOP timeout (0 = block forever)
     * @param worker_id Worker claiming this job
     * @return Job if one was available, nullopt on timeout
     */
    std::optional<Job> pick(const std::vector<std::string>& types, long timeout_sec, const std::string& worker_id = "");

    /**
     * @brief Mark a job as completed with result
     */
    void complete(const std::string& id, const json& result);

    /**
     * @brief Mark a job as failed; requeue if retries remain
     */
    void fail(const std::string& id, const std::string& error);

    /**
     * @brief Send a job STRAIGHT to the dead-letter queue, bypassing the retry
     *        budget — for permanent failures (PermanentJobError, e.g. no handler
     *        for the type) where retrying only burns tight pick→throw→requeue
     *        cycles. Mirrors fail()'s DLQ branch without the retry path.
     */
    void dead_letter(const std::string& id, const std::string& error);

    /**
     * @brief List jobs currently in the dead-letter queue.
     * @param type Filter by job type (empty = all DLQ).
     * @param limit Max results.
     */
    std::vector<Job> list_dlq(const std::string& type = "", int limit = 100);

    /**
     * @brief Move a DLQ job back to its live queue, resetting retry_count.
     * @return true if the job was found and requeued; false otherwise.
     */
    bool requeue_from_dlq(const std::string& id);

    /**
     * @brief Current total DLQ depth across all types.
     */
    long dlq_depth();

    /**
     * @brief Depth per job type. Scans each `jobs:dlq:*` list to return
     *        {type → depth}. Used by the metrics refresher so dashboards
     *        can see which specific job type is clogged.
     */
    std::unordered_map<std::string, long> dlq_depth_by_type();

    /**
     * @brief Live depth per job type for the WAITING queue (`jobs:queue:*`),
     *        as {type → depth}. The leading-indicator counterpart to
     *        dlq_depth_by_type (which is the lagging "already gave up" signal):
     *        a climbing queue depth means submitters are outrunning the
     *        worker pool before anything reaches the DLQ. Used by the metrics
     *        refresher so dashboards/alerts can see a backlog forming.
     */
    std::unordered_map<std::string, long> queue_depth_by_type();

    /**
     * @brief Return any jobs left in this worker's processing list to the
     *        live queue. Call once at worker startup: if the previous
     *        instance with the same WORKER_ID crashed between BRPOPLPUSH
     *        and complete()/fail(), the id stranded in jobs:processing:*
     *        gets rescued and re-executed.
     * @return number of jobs rescued.
     */
    long recover_processing(const std::string& worker_id);

    /**
     * @brief Move every job whose backoff window has elapsed from the delayed
     *        set back onto its live queue. Cheap no-op when backoff is disabled.
     *        Call periodically from the worker loop.
     * @return number of jobs promoted.
     */
    long promote_due_jobs();

    /**
     * @brief Reclaim jobs whose visibility lease has expired — a worker picked
     *        them and died without complete()/fail(), so no same-id startup
     *        recovery will ever rescue them. Each is run back through fail()
     *        (bumps retry, requeues or DLQs), so a permanently-wedged job is not
     *        reaped forever. Cheap no-op when the lease is disabled. Call
     *        periodically from the worker loop.
     * @return number of leases reclaimed.
     */
    long reap_expired_leases();

    /**
     * @brief Get job status by ID
     */
    std::optional<Job> get_status(const std::string& id);

    /**
     * @brief Cancel a job (sets status to failed with "cancelled" error)
     */
    bool cancel(const std::string& id);

    /**
     * @brief One page of the time-ordered job listing plus the index total.
     */
    struct JobPage {
        std::vector<Job> jobs;
        long total = 0;
    };

    /**
     * @brief List jobs newest-first with real offset pagination, backed by
     *        the jobs:index sorted sets (score = created_at).
     * @param type Filter by job type (empty = all types).
     * @param limit Page size.
     * @param offset 0-based start position.
     * @details Index entries whose blob has expired (result_ttl) are healed
     *          lazily: dropped from the queried index as pages touch them,
     *          so `total` may transiently overcount until old pages are
     *          visited. Jobs submitted before the index existed are not
     *          listed (they age out within result_ttl anyway).
     */
    JobPage list_paged(const std::string& type = "", int limit = 20, int offset = 0);

    /**
     * @brief Record the worker's processing-span trace id on the job blob so
     *        UIs can deep-link into the tracing backend. Best-effort no-op
     *        when the job is gone.
     */
    void set_trace_id(const std::string& id, const std::string& trace_id);

    /**
     * @brief Health check — ping Redis
     */
    bool health_check();

    // Ordering: worker_main joins its threads before Core::shutdown() reaches
    // Jobs::shutdown(). Even so, pick() holds a local shared_ptr copy of
    // blocking_client_ for its BRPOP, so dropping the manager's reference here
    // can't free the client mid-pop — the worst case is a benign refcount race,
    // not a use-after-free on the Redis object.
    void shutdown();

    bool is_initialized() const { return initialized_; }
    long result_ttl() const { return result_ttl_; }
    int default_max_retries() const { return default_max_retries_; }

private:
    void check_initialized() const;

    // Drop a job's visibility lease (no-op when the feature is off or the lease
    // is already gone). Called from every terminal/transition path so the reaper
    // never resurrects a job that already moved on.
    void clear_lease_(sw::redis::Redis& redis, const std::string& id);

    /**
     * @brief Load the job blob for @p id in one GET. Returns nullopt when the
     *        key is missing; JSON parse errors propagate (nlohmann exceptions),
     *        exactly like the inlined get→parse it replaces.
     */
    std::optional<Job> load_job_(sw::redis::Redis& redis, const std::string& id);

    /// load_job_, but a missing blob throws — the "Job not found" contract
    /// complete()/fail()/dead_letter() expose to controllers.
    Job load_job_or_throw_(sw::redis::Redis& redis, const std::string& id);

    /// Persist the job blob (plain SET, no TTL) — the write half of load_job_.
    void save_job_(sw::redis::Redis& redis, const Job& job);

    // Drop the job from its worker's processing list (best-effort) and release
    // its visibility lease — the shared terminal-transition cleanup of
    // complete()/fail()/dead_letter(). cancel() keeps its own variant (it also
    // clears the pending queue and gates the lrem on was_processing).
    void release_from_worker_(sw::redis::Redis& redis, const Job& job);

    /**
     * @brief SCAN every list under @p prefix and return {type → LLEN}, where
     *        type is the key with the prefix stripped. Best-effort: swallows
     *        Redis errors ("<log_ctx>: scan failed"). Shared body of
     *        dlq_depth_by_type() / queue_depth_by_type().
     * @param skip_key Exact key to skip (the DLQ `_all` index set; "" = none).
     */
    std::unordered_map<std::string, long> depth_by_prefix_(const std::string& prefix,
                                                           const std::string& skip_key,
                                                           const char* log_ctx);

    // Backoff delay for a job that just failed its @p retry_count-th attempt
    // (>=1). Exponential (base * 2^(retry_count-1)), capped at the configured
    // max. Returns 0 when backoff is disabled → caller requeues immediately.
    int64_t backoff_delay_ms_(int retry_count) const;

    /**
     * @brief Load the Job rows for @p ids in a single MGET round-trip.
     *        Missing or unparseable entries are skipped silently — unless
     *        @p stale is given, in which case their ids are collected there
     *        (list_paged uses that to heal expired index entries).
     */
    std::vector<Job> fetch_jobs_by_ids_(const std::vector<std::string>& ids, std::vector<std::string>* stale = nullptr);
};

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
// The global JobQueue instance (global_jobs) lives in Jobs.cpp; these
// functions are its only doorway — mirrors Billing::'s singleton shape in
// src/billing/PayPalClient.cpp.

void initialize(long result_ttl = 86400, int max_retries = 3);

JobQueue& get();

bool is_initialized();

void shutdown();

}  // namespace Jobs
