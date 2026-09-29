/**
 * @file Jobs.cpp
 * @brief Bodies for src/jobs/Jobs.hpp — compiled once into app_core. This is
 *        the only jobs TU that sees redis++/spdlog/Cache/Trace; the header no
 *        longer exposes them to including TUs. Every contract and its
 *        rationale is documented on the declarations in the header; the
 *        comments here explain the Redis mechanics in place.
 */

#include "jobs/Jobs.hpp"

#include <chrono>
#include <iterator>
#include <stdexcept>
#include <utility>

#include <spdlog/spdlog.h>
#include <sw/redis++/redis++.h>

#include <nlohmann/json.hpp>

#include "cache/Cache.hpp"
#include "observability/Trace.hpp"
#include "utils/Time.hpp"

namespace Jobs {

void JobQueue::initialize(long result_ttl, int max_retries) {
    if (initialized_) {
        throw std::runtime_error("JobQueue already initialized");
    }
    if (!Cache::is_initialized()) {
        throw std::runtime_error("Cache must be initialized before Jobs");
    }
    result_ttl_ = result_ttl;
    default_max_retries_ = max_retries;
    initialized_ = true;
    spdlog::info("Job queue initialized (result_ttl={}s, max_retries={})", result_ttl_, default_max_retries_);
}

void JobQueue::set_retry_backoff(int64_t base_ms, int64_t max_ms) {
    retry_backoff_base_ms_ = base_ms < 0 ? 0 : base_ms;
    retry_backoff_max_ms_ = max_ms < base_ms ? base_ms : max_ms;
}

void JobQueue::init_blocking_client(
    const std::string& host, int port, long brpop_timeout_sec, const std::string& password, int pool_size, int db) {
    const auto sock_to = std::chrono::milliseconds((brpop_timeout_sec + 2) * 1000);
    // One blocking connection is held per concurrent worker thread for the
    // duration of its BRPOP — size the pool to concurrency or threads beyond
    // the 4th stall waiting for a connection (silent throughput cliff).
    blocking_client_ = Cache::make_standalone_client(host, port, pool_size, password, sock_to, sock_to, db);
    blocking_client_->ping();
    spdlog::info("Jobs blocking Redis client initialized ({}:{}, db={}, socket_timeout={}s)",
                 host,
                 port,
                 db,
                 brpop_timeout_sec + 2);
}

void JobQueue::init_blocking_client_sentinel(const std::string& master_name,
                                             const std::vector<std::pair<std::string, int>>& sentinels,
                                             long brpop_timeout_sec,
                                             const std::string& password,
                                             const std::string& sentinel_password,
                                             int pool_size,
                                             int db) {
    const auto sock_to = std::chrono::milliseconds((brpop_timeout_sec + 2) * 1000);
    // Size to worker concurrency — see init_blocking_client.
    blocking_client_ = Cache::make_sentinel_client(
        master_name, sentinels, pool_size, password, sentinel_password, sock_to, sock_to, db);
    blocking_client_->ping();
    spdlog::info("Jobs blocking Redis client initialized via Sentinel (master: {}, db={}, socket_timeout={}s)",
                 master_name,
                 db,
                 brpop_timeout_sec + 2);
}

Job JobQueue::submit(const std::string& type, const json& payload, int max_retries) {
    check_initialized();
    auto& redis = Cache::get().get_client();

    Job job;
    job.id = generate_uuid();
    job.type = type;
    job.payload = payload;
    job.status = "pending";
    job.result = nullptr;
    job.retry_count = 0;
    job.max_retries = (max_retries >= 0) ? max_retries : default_max_retries_;
    job.created_at = now_epoch();
    job.updated_at = job.created_at;
    // Carry the originating request's trace context (if we're inside a
    // request) so the worker can continue the same distributed trace.
    job.traceparent = Observability::Trace::current_traceparent();

    // Store job data
    save_job_(redis, job);

    // Push job ID to the type-specific queue
    redis.lpush(queue_key(type), job.id);

    // Time-ordered indexes for paginated listing. Best-effort: a missed
    // ZADD only means the job won't show in the admin list.
    try {
        const double score = static_cast<double>(job.created_at);
        redis.zadd(index_key(), job.id, score);
        redis.zadd(index_key_for(type), job.id, score);
    } catch (const std::exception& e) {
        spdlog::warn("Jobs: failed to index job {}: {}", job.id, e.what());
    }

    spdlog::debug("Job submitted: id={} type={}", job.id, type);
    return job;
}

std::optional<Job> JobQueue::pick(const std::vector<std::string>& types,
                                  long timeout_sec,
                                  const std::string& worker_id) {
    check_initialized();
    // Local copy keeps the blocking client alive across the BRPOP even if
    // shutdown() resets the member concurrently (see member declaration).
    auto blocking = blocking_client_;
    auto& brpop_redis = blocking ? *blocking : Cache::get().get_client();
    auto& redis = Cache::get().get_client();

    const std::string proc_list = processing_key(worker_id);
    std::string job_id;

    if (types.size() == 1) {
        // BRPOPLPUSH is atomic: id moves from queue → processing list in
        // one round-trip. redis-plus-plus exposes this as brpoplpush.
        auto moved = brpop_redis.brpoplpush(queue_key(types[0]), proc_list, std::chrono::seconds(timeout_sec));
        if (!moved)
            return std::nullopt;
        job_id = *moved;
    } else {
        // Multi-key blocking pop has no atomic move variant in classic
        // Redis (BLMPOP exists in 7.0+ but the client doesn't expose it
        // uniformly). Accept a small window: BRPOP → RPUSH to processing.
        std::vector<std::string> keys;
        keys.reserve(types.size());
        for (const auto& t : types)
            keys.push_back(queue_key(t));
        auto result = brpop_redis.brpop(keys.begin(), keys.end(), timeout_sec);
        if (!result)
            return std::nullopt;
        job_id = result->second;
        try {
            redis.rpush(proc_list, job_id);
        } catch (...) {
            // The id is already off the queue. If we can't record it in the
            // processing list it would be stranded (neither queued nor
            // tracked, so recovery can't rescue it) — put it back on its
            // queue (result->first) instead of silently losing the job.
            try {
                redis.rpush(result->first, job_id);
            } catch (...) {}
            spdlog::warn("Job {} pick: failed to record in processing list — requeued", job_id);
            return std::nullopt;
        }
    }

    // Fetch job data
    auto data = redis.get(job_key(job_id));
    if (!data) {
        spdlog::warn("Job {} popped from queue but data not found in Redis", job_id);
        // Clean up — ghost id shouldn't linger in processing list.
        try {
            redis.lrem(proc_list, 0, job_id);
        } catch (...) {}
        return std::nullopt;
    }

    auto job = Job::from_json(json::parse(*data));
    job.status = "processing";
    job.worker_id = worker_id;
    job.updated_at = now_epoch();

    // Update status in Redis
    save_job_(redis, job);

    // Lease the job so a cross-worker reaper can reclaim it if this worker
    // dies before complete()/fail(). Best-effort: a missed lease only loses
    // the visibility-timeout safety net, not the job (startup recovery still
    // covers a same-id restart).
    if (visibility_timeout_ms_ > 0) {
        try {
            redis.zadd(
                leases_key(), job.id, static_cast<double>(Utils::Time::now_epoch_millis() + visibility_timeout_ms_));
        } catch (...) {}
    }

    spdlog::debug("Job picked: id={} type={} worker={}", job.id, job.type, worker_id);
    return job;
}

void JobQueue::complete(const std::string& id, const json& result) {
    check_initialized();
    auto& redis = Cache::get().get_client();

    auto job = load_job_or_throw_(redis, id);
    job.status = "completed";
    job.result = result;
    job.updated_at = now_epoch();

    redis.setex(job_key(id), result_ttl_, job.to_json().dump());
    // Drop from the worker's processing list — the job is officially done
    // and no recovery pass should resurrect it.
    release_from_worker_(redis, job);
    spdlog::debug("Job completed: id={}", id);
}

void JobQueue::fail(const std::string& id, const std::string& error) {
    check_initialized();
    auto& redis = Cache::get().get_client();

    auto job = load_job_or_throw_(redis, id);
    job.retry_count++;
    job.updated_at = now_epoch();

    // Remove from the worker's processing list whether we requeue or DLQ —
    // recovery should never double-process a job that already failed once.
    release_from_worker_(redis, job);

    if (job.retry_count < job.max_retries) {
        // Requeue for retry.
        job.status = "pending";
        job.error = error;
        save_job_(redis, job);
        const int64_t delay_ms = backoff_delay_ms_(job.retry_count);
        if (delay_ms > 0) {
            // Park in the delayed set; promote_due_jobs() returns it to the
            // live queue once the backoff window elapses. Avoids a tight
            // pick→fail→pick loop when the queue is otherwise empty.
            redis.zadd(delayed_key(), id, static_cast<double>(Utils::Time::now_epoch_millis() + delay_ms));
            spdlog::info(
                "Job {} retry {}/{} scheduled in {}ms: {}", id, job.retry_count, job.max_retries, delay_ms, error);
        } else {
            redis.lpush(queue_key(job.type), id);
            spdlog::info("Job {} requeued (retry {}/{}): {}", id, job.retry_count, job.max_retries, error);
        }
    } else {
        // Max retries exceeded — send to dead-letter queue for inspection.
        // DLQ entries are NOT reaped by result_ttl_ (operator must drain
        // or requeue them explicitly via /api/v1/jobs/dlq/{id}/requeue).
        job.status = "dead";
        job.error = error;
        save_job_(redis, job);
        redis.lpush(dlq_key(job.type), id);
        redis.sadd(kDlqAllKey, id);  // index for "list all DLQ" queries
        spdlog::warn("Job {} DLQ'd after {} retries: {}", id, job.retry_count, error);
    }
}

void JobQueue::dead_letter(const std::string& id, const std::string& error) {
    check_initialized();
    auto& redis = Cache::get().get_client();
    auto job = load_job_or_throw_(redis, id);
    job.updated_at = now_epoch();
    release_from_worker_(redis, job);
    job.status = "dead";
    job.error = error;
    // Index BEFORE committing the blob: the 3 writes aren't atomic, and if
    // lpush/sadd threw after the blob was already marked "dead" the job
    // would vanish from list_dlq/requeue/recover entirely. Indexing first
    // means a failure leaves it discoverable (with its prior status) and
    // list_dlq heals once the blob write lands.
    redis.lpush(dlq_key(job.type), id);
    redis.sadd(kDlqAllKey, id);
    save_job_(redis, job);
    spdlog::warn("Job {} sent straight to DLQ (permanent failure): {}", id, error);
}

std::vector<Job> JobQueue::list_dlq(const std::string& type, int limit) {
    check_initialized();
    auto& redis = Cache::get().get_client();
    std::vector<std::string> ids;
    if (type.empty()) {
        // Scan the index set.
        redis.smembers(kDlqAllKey, std::back_inserter(ids));
    } else {
        redis.lrange(dlq_key(type), 0, limit - 1, std::back_inserter(ids));
    }
    if (static_cast<int>(ids.size()) > limit)
        ids.resize(limit);
    // One MGET instead of a GET per id (avoids N round-trips).
    return fetch_jobs_by_ids_(ids);
}

bool JobQueue::requeue_from_dlq(const std::string& id) {
    check_initialized();
    auto& redis = Cache::get().get_client();
    Job job;
    try {
        auto loaded = load_job_(redis, id);
        if (!loaded)
            return false;
        job = std::move(*loaded);
    } catch (const json::exception&) {
        // Unparseable blob — same "not requeueable" answer as a missing one.
        // Redis errors still propagate (see the class exception contract).
        return false;
    }
    if (job.status != "dead")
        return false;

    // Remove from per-type DLQ list + index.
    redis.lrem(dlq_key(job.type), 0, id);
    redis.srem(kDlqAllKey, id);

    job.status = "pending";
    job.retry_count = 0;
    job.error.clear();
    job.updated_at = now_epoch();
    save_job_(redis, job);
    redis.lpush(queue_key(job.type), id);
    spdlog::info("Job {} requeued from DLQ", id);
    return true;
}

long JobQueue::dlq_depth() {
    if (!initialized_)
        return 0;
    try {
        return static_cast<long>(Cache::get().get_client().scard(kDlqAllKey));
    } catch (...) {
        return 0;
    }
}

std::unordered_map<std::string, long> JobQueue::dlq_depth_by_type() {
    return depth_by_prefix_("jobs:dlq:", kDlqAllKey, "dlq_depth_by_type");
}

std::unordered_map<std::string, long> JobQueue::queue_depth_by_type() {
    return depth_by_prefix_("jobs:queue:", "", "queue_depth_by_type");
}

long JobQueue::recover_processing(const std::string& worker_id) {
    check_initialized();
    if (worker_id.empty())
        return 0;
    auto& redis = Cache::get().get_client();
    const std::string proc = processing_key(worker_id);
    long recovered = 0;
    // Snapshot the list, push each id back onto its type queue, then drop
    // the processing list. Not atomic across commands, but run during
    // startup only — the worker hasn't picked anything yet, so nothing
    // else writes here.
    std::vector<std::string> ids;
    try {
        redis.lrange(proc, 0, -1, std::back_inserter(ids));
    } catch (...) {
        return 0;
    }
    for (const auto& id : ids) {
        try {
            auto loaded = load_job_(redis, id);
            if (!loaded)
                continue;
            Job job = std::move(*loaded);
            // Put it back on the live queue so another worker (or this one
            // after restart) picks it up again. Don't touch retry_count —
            // the job didn't fail, we just missed its completion.
            job.status = "pending";
            job.worker_id.clear();
            job.updated_at = now_epoch();
            save_job_(redis, job);
            redis.lpush(queue_key(job.type), id);
            ++recovered;
        } catch (...) {}
    }
    try {
        redis.del(proc);
    } catch (...) {}
    if (recovered > 0) {
        spdlog::warn("Jobs: recovered {} orphaned jobs from {}", recovered, proc);
    }
    return recovered;
}

long JobQueue::promote_due_jobs() {
    if (!initialized_ || retry_backoff_base_ms_ <= 0)
        return 0;
    auto& redis = Cache::get().get_client();
    const auto now_ms = Utils::Time::now_epoch_millis();
    std::vector<std::string> ids;
    try {
        redis.zrangebyscore(
            delayed_key(),
            sw::redis::BoundedInterval<double>(0.0, static_cast<double>(now_ms), sw::redis::BoundType::CLOSED),
            std::back_inserter(ids));
    } catch (...) {
        return 0;
    }
    long moved = 0;
    for (const auto& id : ids) {
        try {
            // ZREM-gate: only the caller that actually removes the id pushes
            // it, so concurrent workers never double-promote the same job.
            if (redis.zrem(delayed_key(), id) == 0)
                continue;
            auto job = load_job_(redis, id);
            if (!job)
                continue;  // blob expired — drop the orphaned delayed entry
            redis.lpush(queue_key(job->type), id);
            ++moved;
        } catch (...) {}
    }
    return moved;
}

long JobQueue::reap_expired_leases() {
    if (!initialized_ || visibility_timeout_ms_ <= 0)
        return 0;
    auto& redis = Cache::get().get_client();
    const auto now_ms = Utils::Time::now_epoch_millis();
    std::vector<std::string> ids;
    try {
        redis.zrangebyscore(
            leases_key(),
            sw::redis::BoundedInterval<double>(0.0, static_cast<double>(now_ms), sw::redis::BoundType::CLOSED),
            std::back_inserter(ids));
    } catch (...) {
        return 0;
    }
    long reaped = 0;
    for (const auto& id : ids) {
        try {
            // ZREM-gate so exactly one reaper handles each expired lease.
            if (redis.zrem(leases_key(), id) == 0)
                continue;
            fail(id, "visibility timeout exceeded");
            ++reaped;
        } catch (...) {}
    }
    return reaped;
}

std::optional<Job> JobQueue::get_status(const std::string& id) {
    check_initialized();
    auto& redis = Cache::get().get_client();

    return load_job_(redis, id);
}

bool JobQueue::cancel(const std::string& id) {
    check_initialized();
    auto& redis = Cache::get().get_client();

    auto loaded = load_job_(redis, id);
    if (!loaded) {
        return false;
    }

    auto job = std::move(*loaded);
    // Terminal states can't be cancelled. The real vocabulary is
    // pending/processing/completed/dead (+ "failed", which only cancel()
    // itself sets). The old guard checked "failed" but not "dead", so a DLQ
    // ("dead") job could be cancelled — that TTL-expired its blob while the
    // id stayed indexed in the DLQ set, leaving a dangling entry.
    if (job.status == "completed" || job.status == "dead" || job.status == "failed") {
        return false;
    }

    const bool was_processing = (job.status == "processing");
    job.status = "failed";
    job.error = "cancelled";
    job.updated_at = now_epoch();

    redis.setex(job_key(id), result_ttl_, job.to_json().dump());

    // Drop it from wherever it currently lives: the pending queue, or — if a
    // worker already claimed it — that worker's processing list, so crash
    // recovery won't resurrect a cancelled job.
    redis.lrem(queue_key(job.type), 0, id);
    if (was_processing && !job.worker_id.empty())
        redis.lrem(processing_key(job.worker_id), 0, id);
    clear_lease_(redis, id);

    spdlog::debug("Job cancelled: id={}", id);
    return true;
}

JobQueue::JobPage JobQueue::list_paged(const std::string& type, int limit, int offset) {
    check_initialized();
    auto& redis = Cache::get().get_client();
    const std::string key = type.empty() ? index_key() : index_key_for(type);

    std::vector<std::string> ids;
    redis.zrevrange(key, offset, offset + limit - 1, std::back_inserter(ids));

    JobPage page;
    if (!ids.empty()) {
        std::vector<std::string> stale;
        page.jobs = fetch_jobs_by_ids_(ids, &stale);
        // Heal: blob expired → drop the id from the queried index (and,
        // when the type is known, from the global one too; global-query
        // staleness in typed indexes heals on their own queries).
        for (const auto& id : stale) {
            try {
                redis.zrem(key, id);
                if (!type.empty())
                    redis.zrem(index_key(), id);
            } catch (...) {}
        }
    }
    page.total = static_cast<long>(redis.zcard(key));
    return page;
}

void JobQueue::set_trace_id(const std::string& id, const std::string& trace_id) {
    check_initialized();
    auto& redis = Cache::get().get_client();
    std::optional<Job> job;
    try {
        job = load_job_(redis, id);
    } catch (const json::exception&) {
        return;  // unparseable blob — best-effort no-op
    }
    if (!job)
        return;
    try {
        job->trace_id = trace_id;
        job->updated_at = now_epoch();
        save_job_(redis, *job);
    } catch (...) {}
}

bool JobQueue::health_check() {
    if (!initialized_)
        return false;
    try {
        Cache::get().get_client().ping();
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Jobs health check failed: {}", e.what());
        return false;
    }
}

void JobQueue::shutdown() {
    if (initialized_) {
        spdlog::info("Shutting down job queue");
        blocking_client_.reset();
        initialized_ = false;
    }
}

void JobQueue::check_initialized() const {
    if (!initialized_) {
        throw std::runtime_error("JobQueue not initialized");
    }
}

void JobQueue::clear_lease_(sw::redis::Redis& redis, const std::string& id) {
    if (visibility_timeout_ms_ <= 0)
        return;
    try {
        redis.zrem(leases_key(), id);
    } catch (...) {}
}

std::optional<Job> JobQueue::load_job_(sw::redis::Redis& redis, const std::string& id) {
    auto data = redis.get(job_key(id));
    if (!data)
        return std::nullopt;
    return Job::from_json(json::parse(*data));
}

Job JobQueue::load_job_or_throw_(sw::redis::Redis& redis, const std::string& id) {
    auto job = load_job_(redis, id);
    if (!job)
        throw std::runtime_error("Job not found: " + id);
    return std::move(*job);
}

void JobQueue::save_job_(sw::redis::Redis& redis, const Job& job) {
    redis.set(job_key(job.id), job.to_json().dump());
}

void JobQueue::release_from_worker_(sw::redis::Redis& redis, const Job& job) {
    if (!job.worker_id.empty()) {
        try {
            redis.lrem(processing_key(job.worker_id), 0, job.id);
        } catch (...) {}
    }
    clear_lease_(redis, job.id);
}

std::unordered_map<std::string, long> JobQueue::depth_by_prefix_(const std::string& prefix,
                                                                 const std::string& skip_key,
                                                                 const char* log_ctx) {
    std::unordered_map<std::string, long> out;
    if (!initialized_)
        return out;
    try {
        auto& redis = Cache::get().get_client();
        const std::string pattern = prefix + "*";
        long long cursor = 0;
        do {
            std::vector<std::string> batch;
            cursor = redis.scan(cursor, pattern, 100, std::back_inserter(batch));
            for (auto& k : batch) {
                if (!skip_key.empty() && k == skip_key)
                    continue;  // skip the `_all` index set
                long long len = redis.llen(k);
                std::string type = k.rfind(prefix, 0) == 0 ? k.substr(prefix.size()) : k;
                out[std::move(type)] = static_cast<long>(len);
            }
        } while (cursor != 0);
    } catch (const std::exception& e) {
        spdlog::warn("{}: scan failed: {}", log_ctx, e.what());
    }
    return out;
}

int64_t JobQueue::backoff_delay_ms_(int retry_count) const {
    if (retry_backoff_base_ms_ <= 0)
        return 0;
    int shift = retry_count > 0 ? retry_count - 1 : 0;
    if (shift > 30)
        shift = 30;  // guard the shift against overflow
    int64_t delay = retry_backoff_base_ms_ << shift;
    if (delay <= 0 || delay > retry_backoff_max_ms_)
        delay = retry_backoff_max_ms_;
    return delay;
}

std::vector<Job> JobQueue::fetch_jobs_by_ids_(const std::vector<std::string>& ids, std::vector<std::string>* stale) {
    std::vector<Job> jobs;
    if (ids.empty())
        return jobs;
    auto& redis = Cache::get().get_client();
    std::vector<std::string> keys;
    keys.reserve(ids.size());
    for (const auto& id : ids)
        keys.push_back(job_key(id));
    std::vector<sw::redis::OptionalString> vals;
    vals.reserve(keys.size());
    redis.mget(keys.begin(), keys.end(), std::back_inserter(vals));
    jobs.reserve(vals.size());
    for (size_t i = 0; i < vals.size(); ++i) {
        if (!vals[i]) {
            if (stale)
                stale->push_back(ids[i]);
            continue;
        }
        try {
            jobs.push_back(Job::from_json(json::parse(*vals[i])));
        } catch (...) {
            if (stale)
                stale->push_back(ids[i]);
        }
    }
    return jobs;
}

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

namespace {
/// Global job queue instance — module-private; reach it via the free
/// functions below (mirrors global_paypal_client in PayPalClient.cpp).
std::unique_ptr<JobQueue> global_jobs = nullptr;
}  // namespace

void initialize(long result_ttl, int max_retries) {
    if (global_jobs != nullptr) {
        throw std::runtime_error("JobQueue already initialized");
    }
    global_jobs = std::make_unique<JobQueue>();
    global_jobs->initialize(result_ttl, max_retries);
}

JobQueue& get() {
    if (global_jobs == nullptr) {
        throw std::runtime_error("JobQueue not initialized");
    }
    return *global_jobs;
}

bool is_initialized() {
    return global_jobs != nullptr && global_jobs->is_initialized();
}

void shutdown() {
    if (global_jobs) {
        global_jobs->shutdown();
        global_jobs.reset();
    }
}

}  // namespace Jobs
