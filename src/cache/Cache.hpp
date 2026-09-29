/**
 * @file Cache.hpp
 * @brief Cache module for Redis integration
 * @details Provides Redis operations with Sentinel support for high availability
 *          using redis-plus-plus library
 *
 * Non-template bodies (URL/sentinel parsing, client construction, the
 * CacheManager wrappers and the global-instance lifecycle incl. the test
 * seam) live in Cache.cpp (compiled once into app_core; ADR 0003 as amended
 * 2026-08-22). The guarded_ member template and the cached<T> read-through
 * helper are templates and stay here — the spdlog / redis++ /
 * nlohmann-json includes are load-bearing for their bodies.
 */

#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>
#include <sw/redis++/redis++.h>

#include <nlohmann/json.hpp>

namespace Cache {

using namespace sw::redis;
using namespace std::chrono_literals;

struct RedisAddress {
    std::string host = "127.0.0.1";
    int port = 6379;
};

/**
 * @brief Parse a Redis connection string ("tcp://host:port"; scheme and port
 *        optional). Shared by Cache::initialize and the worker's blocking
 *        client so the parsing (and its error handling) lives in one place.
 * @throws std::runtime_error on a malformed port.
 */
RedisAddress parse_redis_url(const std::string& url);

/**
 * @brief Parse a "host:port,host:port" Sentinel node list. Entries without
 *        a valid port are skipped with a warning rather than aborting boot.
 */
std::vector<std::pair<std::string, int>> parse_sentinel_nodes_csv(const std::string& csv);

/**
 * @brief Build a standalone Redis client. Shared between Cache and the
 *        Jobs blocking-BRPOP client so connection params live in one place.
 */
std::unique_ptr<Redis> make_standalone_client(const std::string& host,
                                              int port,
                                              size_t pool_size,
                                              const std::string& password,
                                              std::chrono::milliseconds socket_timeout,
                                              std::chrono::milliseconds pool_wait_timeout,
                                              int db = 0);

/**
 * @brief Build a Sentinel-aware Redis client. Shared between Cache and Jobs.
 */
std::unique_ptr<Redis> make_sentinel_client(const std::string& master_name,
                                            const std::vector<std::pair<std::string, int>>& sentinels,
                                            size_t pool_size,
                                            const std::string& password,
                                            const std::string& sentinel_password,
                                            std::chrono::milliseconds socket_timeout,
                                            std::chrono::milliseconds pool_wait_timeout,
                                            int db = 0);

/**
 * @brief Redis cache manager with Sentinel support
 *
 * Error contract: the wrapper methods (get/set/del/exists/expire/ttl/
 * sadd/smembers/zadd/publish/...) are FAIL-OPEN — they swallow
 * sw::redis::Error, log a warning and return a false/empty default, so a
 * Redis outage degrades features instead of failing requests. Callers that
 * need the failure signal must check the RETURN VALUE (see
 * AuthController::mint_session). Two deliberate exceptions: incr()/decr()
 * RETHROW — counters silently stuck at a default would corrupt rate
 * accounting. Direct get_client() calls bypass all of this: wrap them in
 * try/catch yourself.
 */
class CacheManager {
private:
    std::unique_ptr<Redis> redis_client;
    bool initialized = false;

public:
    // Polymorphic so tests can substitute an in-memory fake (see
    // tests/InMemoryCache.hpp) for the singleton via
    // Cache::install_for_testing — the data ops below are virtual.
    virtual ~CacheManager() = default;

    /**
     * @brief Initialize Redis cache
     * @param connection_str Redis connection string (e.g., "tcp://127.0.0.1:6379")
     * @param pool_size Connection pool size
     */
    void initialize(const std::string& connection_str,
                    size_t pool_size = 10,
                    const std::string& password = "",
                    std::chrono::milliseconds socket_timeout = 500ms,
                    std::chrono::milliseconds pool_wait_timeout = 500ms,
                    int db = 0);

    /**
     * @brief Initialize Redis with Sentinel for high availability
     * @param master_name Master service name
     * @param sentinels Vector of sentinel addresses (host, port)
     * @param pool_size Connection pool size
     */
    void initialize_with_sentinel(const std::string& master_name,
                                  const std::vector<std::pair<std::string, int>>& sentinels,
                                  size_t pool_size = 10,
                                  const std::string& password = "",
                                  const std::string& sentinel_password = "",
                                  std::chrono::milliseconds socket_timeout = 500ms,
                                  std::chrono::milliseconds pool_wait_timeout = 500ms,
                                  int db = 0);

    virtual bool set(const std::string& key, const std::string& value, long ttl = 0);

    /**
     * @brief Atomic SET-if-not-exists with TTL — used as a lightweight
     *        distributed lock primitive. Returns true if the caller now
     *        holds the key, false if another writer got there first.
     *        On Redis error returns false (treat as "lock not acquired"
     *        to avoid double-processing during outages).
     */
    virtual bool set_nx(const std::string& key, const std::string& value, std::chrono::milliseconds ttl);

    virtual std::optional<std::string> get(const std::string& key);

    virtual long del(const std::string& key);

    virtual long del(const std::vector<std::string>& keys);

    virtual bool exists(const std::string& key);

    virtual bool expire(const std::string& key, long seconds);

    virtual long ttl(const std::string& key);

    // NOT fail-open (see class contract): counters silently stuck at a
    // default would corrupt rate accounting, so incr/decr log and RETHROW.
    // INCRBY key 1 is exactly INCR key, so a single command suffices.
    virtual long long incr(const std::string& key, long long increment = 1);

    virtual long long decr(const std::string& key, long long decrement = 1);

    long sadd(const std::string& key, const std::string& member);

    std::vector<std::string> smembers(const std::string& key);

    long zadd(const std::string& key, const std::string& member, double score);

    long long publish(const std::string& channel, const std::string& message);

    bool health_check();

    void shutdown();

    virtual bool is_initialized() const { return initialized; }

    Redis& get_client();

private:
    void check_initialized() const;

    /**
     * @brief Shared body of the FAIL-OPEN wrappers above: require an
     *        initialized client, run @p fn, and on sw::redis::Error log
     *        @p err_fmt (context args + e.what()) and return @p fallback.
     *        check_initialized() still THROWS — fail-open covers Redis
     *        errors, not use-before-init. incr/decr deliberately do not
     *        route through here (they rethrow — see the class contract).
     */
    template <typename R, typename Fn, typename... Args>
    R guarded_(R fallback, Fn&& fn, spdlog::format_string_t<Args..., const char*> err_fmt, Args&&... args) {
        check_initialized();
        try {
            return fn();
        } catch (const Error& e) {
            spdlog::error(err_fmt, std::forward<Args>(args)..., e.what());
            return fallback;
        }
    }
};

/**
 * @brief Initialize the global cache instance
 */
void initialize(const std::string& connection_str,
                size_t pool_size = 10,
                const std::string& password = "",
                std::chrono::milliseconds socket_timeout = 500ms,
                std::chrono::milliseconds pool_wait_timeout = 500ms,
                int db = 0);

void initialize_with_sentinel(const std::string& master_name,
                              const std::vector<std::pair<std::string, int>>& sentinels,
                              size_t pool_size = 10,
                              const std::string& password = "",
                              const std::string& sentinel_password = "",
                              std::chrono::milliseconds socket_timeout = 500ms,
                              std::chrono::milliseconds pool_wait_timeout = 500ms,
                              int db = 0);

CacheManager& get();

bool is_initialized();

void shutdown();

// ── Test seam ────────────────────────────────────────────────────────────────
// Swap the global cache for a fake (CacheManager subclass, e.g.
// tests/InMemoryCache.hpp) so cache-aside and fail-open paths are
// unit-testable without a live Redis. Call reset_for_testing() in TearDown.
void install_for_testing(std::unique_ptr<CacheManager> fake);
void reset_for_testing();

// ── Read-through (cache-aside) helper ─────────────────────────────────────────
// Return the cached value for `key`, or call `loader`, cache its result for
// `ttl_sec`, and return it. T must be nlohmann-serializable (to_json/from_json
// via ADL — every Domain DTO already is). Centralizes the get→miss→load→set
// dance so each call site (and each fork) doesn't hand-roll it differently.
//
// FAIL-OPEN by design: if the cache is uninitialized/down, or the cached blob is
// unparseable (e.g. the DTO's shape changed across a deploy), fall through to
// loader() and just skip caching — correctness never depends on Redis. Only
// cache positive lookups here; callers that must cache "absent" should wrap T in
// std::optional and let from_json handle null.
template <typename T, typename Loader>
T cached(const std::string& key, long ttl_sec, Loader&& loader) {
    if (is_initialized()) {
        try {
            if (auto hit = get().get(key))
                return nlohmann::json::parse(*hit).template get<T>();
        } catch (const std::exception& e) {
            spdlog::debug("cache: ignoring unusable entry for '{}' ({})", key, e.what());
        }
    }
    T value = std::forward<Loader>(loader)();
    if (is_initialized()) {
        try {
            get().set(key, nlohmann::json(value).dump(), ttl_sec);
        } catch (const std::exception& e) {
            spdlog::debug("cache: failed to store '{}' ({})", key, e.what());
        }
    }
    return value;
}

}  // namespace Cache
