/**
 * @file Core.hpp
 * @brief Core application module
 * @details Orchestrates initialization and shutdown of all subsystems.
 *
 * Declarations only — the bodies live in Core.cpp (compiled once into
 * app_core; ADR 0003 as amended 2026-08-22): including this header no
 * longer pulls the 13 subsystem headers (database/pqxx, cache/redis++,
 * jobs, Kafka, PayPal, mailer, storage, OTel/prometheus, ...) into the
 * including TU — those includes moved to Core.cpp. Only the binary entry
 * points and api/HealthController.hpp may include this header
 * (check-module-deps rule 2); everyone else consults the tiny
 * core/Modules.hpp for the module switches / shutdown flag.
 */

#pragma once

#include <functional>
#include <initializer_list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/Modules.hpp"
#include "utils/Config.hpp"

namespace Core {

/**
 * @brief Surface insecure database credentials at startup.
 * @details Parses a postgresql:// URL and extracts the password component.
 *          If the password is empty or matches a known-weak default
 *          ("postgres", "password", etc.), logs a loud warn. We can't
 *          hard-fail by default because docker-compose ships
 *          postgres:postgres for the local dev stack — flip
 *          DATABASE_REQUIRE_SECURE_PASSWORD=true in any non-dev
 *          environment and the check becomes a hard error.
 *          libpq key=value-form connection strings and URLs without
 *          userinfo (peer auth, certificate auth) are skipped.
 */
void check_password_value(const std::string& password);

void check_password_safety(const std::string& url);

/**
 * @brief Initialization mode
 */
enum class InitMode {
    Full,        // API server: all subsystems
    Worker,      // Worker process: skip Tasks, skip Messaging
    MigrateOnly  // Run migrations only: Config + Observability + Database + Migrations
};

/**
 * @brief Application lifecycle manager
 */
class Application {
private:
    bool initialized = false;
    std::string config_file;
    InitMode init_mode = InitMode::Full;

public:
    void initialize(const std::string& config_path, InitMode mode = InitMode::Full);

    // Exposed so the prod-safety gate can be unit-tested without a full boot.
    static void validate_config(Config::AppConfig& cfg);

private:
    // ---------------------------------------------------------------------
    // Per-subsystem initializers — each reads from config and brings up
    // exactly one module. Kept in initialize()'s lexical order for clarity;
    // bodies (and every rationale comment) live in Core.cpp.
    // ---------------------------------------------------------------------

    // Boot-time config sanity. A 12-factor app reads everything from env, so a
    // single typo can silently flip a security control. Fail LOUD (throw →
    // caught in initialize(), logged, process exits non-zero) on combinations
    // that are almost always mistakes in production, rather than starting up
    // quietly insecure. The auth.mode=jwt secret check lives in init_security_.
    static void validate_config_(Config::AppConfig& cfg);

    static void init_observability_(Config::AppConfig& cfg);

    // Shared JSON fallback for the env-first string lists (replica URLs,
    // Kafka topics): walk @p path segments with at() so ANY missing segment
    // lands in the same catch. Returns false when the walk/conversion threw,
    // so callers can apply their own default. (read_sentinels_ stays
    // separate: it reads host/port PAIRS, not strings.)
    static bool read_string_array_(Config::AppConfig& cfg,
                                   std::initializer_list<const char*> path,
                                   std::vector<std::string>& out);

    // DATABASE_REPLICA_URLS (env) is preferred for container deployments;
    // config JSON is a fallback for local dev where setting env is annoying.
    static std::vector<std::string> read_replicas_(Config::AppConfig& cfg);

    static void init_database_(Config::AppConfig& cfg);

    static void init_migrations_(Config::AppConfig& cfg, InitMode mode);

    static std::vector<std::pair<std::string, int>> read_sentinels_(Config::AppConfig& cfg);

    static void init_cache_(Config::AppConfig& cfg);

    static std::vector<std::string> read_kafka_topics_(Config::AppConfig& cfg);

    static void init_messaging_(Config::AppConfig& cfg);

    // Throws if auth.mode=jwt and no secret is set — refuse to silently start
    // a service that would accept unauthenticated traffic.
    static void init_security_();

    // Shared registrar for the two jobs depth gauges (DLQ + waiting queue):
    // identical bookkeeping over a different Redis keyspace. Registers
    // @p gauge_name as a Prometheus gauge, labeled by job type (special label
    // value `_total` carries the aggregate across every type for single-stat
    // widgets), refreshed every N seconds from Redis via @p depth_fn.
    using DepthFn = std::unordered_map<std::string, long> (*)();
    static void register_depth_metric_(Config::AppConfig& cfg,
                                       const char* gauge_name,
                                       const char* gauge_help,
                                       const char* refresh_conf_key,
                                       const char* refresh_env_var,
                                       const char* task_name,
                                       DepthFn depth_fn);

    // Registers jobs_dlq_depth — the lagging "already gave up" signal:
    // operators can spot which queue specifically is clogged.
    static void register_dlq_metric_(Config::AppConfig& cfg);

    // Registers jobs_queue_depth — the LEADING indicator of saturation: a
    // climbing waiting-queue means submitters are outrunning the worker pool,
    // visible long before anything lands in the DLQ. Same bookkeeping as the
    // DLQ gauge, over jobs:queue:* instead of jobs:dlq:*.
    static void register_queue_depth_metric_(Config::AppConfig& cfg);

    // Registers db_pool_active_connections + db_pool_size gauges, labeled by
    // pool (primary/replica). Saturation = active / size → 1.0 means acquire()
    // is about to start timing out; it's the cause the HighP99Latency alert
    // tells operators to check first.
    static void register_db_pool_metric_(Config::AppConfig& cfg);

    // Registers db_replica_lag_seconds — how far a read replica trails the
    // primary, in seconds. Only registered when replicas are configured.
    static void register_replication_lag_metric_(Config::AppConfig& cfg);

    // Periodically prune expired single-use token nonces (used_tokens,
    // migration 002). Unlike the old Redis TTL nonce these rows are permanent,
    // so without a reaper the table + its index grow monotonically.
    static void register_token_reaper_();

    // Schedules the transactional-outbox drain (Jobs::Outbox::drain) every
    // outbox.drain_interval_sec seconds. Opt-in: the default 0 schedules
    // nothing, so the outbox table sits inert unless a deploy turns it on.
    // Server-mode only (Tasks isn't initialized in the worker).
    static void register_outbox_drain_(Config::AppConfig& cfg);

    // Registers the subsystem probes the template ships with. Services
    // that add their own modules can call Core::get().register_health_check
    // at any point after Core::initialize() returns.
    void register_default_health_checks_();

    static void init_jobs_(Config::AppConfig& cfg);

public:
    using HealthFn = std::function<bool()>;

    /**
     * @brief Register a health probe. The passed callable is invoked each
     *        time /health or /ready rolls through the list.
     * @param critical When true (default) a failing probe makes /ready report
     *        NotReady (kube pulls the pod from rotation). When false the probe
     *        is "degraded": still surfaced in /health, but a failure does NOT
     *        fail readiness — for optional dependencies (SMTP, object storage,
     *        Kafka) whose outage shouldn't take the whole service out of
     *        rotation. Thread-safe; typically called once during subsystem init.
     */
    void register_health_check(std::string name, HealthFn probe, bool critical = true);

    struct ComponentHealth {
        std::string name;
        bool initialized;
        bool healthy;
        bool critical;
    };

    /**
     * @brief Report every registered probe's current state. Used by
     *        /health to produce the per-component breakdown.
     */
    std::vector<ComponentHealth> health_report();

    /**
     * @brief True iff every CRITICAL probe currently passes. Degraded
     *        (non-critical) probes are ignored — they show up in /health but
     *        never fail readiness. No `initialized` guard, so it's unit-testable
     *        directly; health_check() adds that guard for the live /ready path.
     */
    bool all_critical_healthy();

    bool health_check() { return initialized && all_critical_healthy(); }

private:
    struct HealthEntry {
        std::string name;
        HealthFn probe;
        bool critical = true;
    };
    std::vector<HealthEntry> health_checks_;
    std::mutex health_mu_;

public:
    void shutdown();

    bool is_initialized() const { return initialized; }

    void reload_config();

    // Generated from project(VERSION ...) in CMakeLists.txt — the single
    // place the version number lives (Core.cpp includes version.hpp).
    std::string version() const;
};

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
// The global Application instance (global_app) lives in Core.cpp; these
// functions are its only doorway — mirrors Jobs::'s singleton shape in
// src/jobs/Jobs.cpp.

void initialize(const std::string& config_path, InitMode mode = InitMode::Full);

Application& get();

bool is_initialized();

void shutdown();

bool health_check();

// Feature-module switches (the Core::<module>_enabled() accessors) and
// the shutdown flag (Core::is_shutting_down) live in core/Modules.hpp —
// included above — so controllers can consult them without pulling in this
// composition root.

}  // namespace Core
