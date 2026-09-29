/**
 * @file Cache.cpp
 * @brief Bodies for src/cache/Cache.hpp — compiled once into app_core:
 *        connection-string/sentinel parsing, client construction, the
 *        fail-open CacheManager wrappers and the global-instance lifecycle
 *        incl. the test seam. The guarded_ member template and the cached<T>
 *        read-through helper stay in the header; every contract is
 *        documented on the declarations there.
 */

#include "cache/Cache.hpp"

#include <iterator>
#include <stdexcept>

#include <spdlog/spdlog.h>

#include "utils/Strings.hpp"

namespace Cache {

RedisAddress parse_redis_url(const std::string& url) {
    RedisAddress out;
    std::string addr = url;
    if (addr.starts_with("tcp://"))
        addr = addr.substr(6);
    auto colon = addr.find(':');
    if (colon == std::string::npos) {
        if (!addr.empty())
            out.host = addr;
        return out;
    }
    out.host = addr.substr(0, colon);
    const std::string port_str = addr.substr(colon + 1);
    try {
        out.port = std::stoi(port_str);
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid Redis port in connection string: '" + port_str + "'");
    }
    return out;
}

std::vector<std::pair<std::string, int>> parse_sentinel_nodes_csv(const std::string& csv) {
    std::vector<std::pair<std::string, int>> out;
    for (const auto& node : Utils::Strings::split_csv_vec(csv)) {
        auto colon = node.find(':');
        if (colon == std::string::npos) {
            spdlog::warn("Sentinel node '{}' has no port — skipping", node);
            continue;
        }
        try {
            out.emplace_back(node.substr(0, colon), std::stoi(node.substr(colon + 1)));
        } catch (const std::exception&) {
            spdlog::warn("Sentinel node '{}' has an invalid port — skipping", node);
        }
    }
    return out;
}

std::unique_ptr<Redis> make_standalone_client(const std::string& host,
                                              int port,
                                              size_t pool_size,
                                              const std::string& password,
                                              std::chrono::milliseconds socket_timeout,
                                              std::chrono::milliseconds pool_wait_timeout,
                                              int db) {
    ConnectionOptions opts;
    opts.host = host;
    opts.port = port;
    opts.db = db;
    opts.socket_timeout = socket_timeout;
    if (!password.empty())
        opts.password = password;
    ConnectionPoolOptions pool_opts;
    pool_opts.size = pool_size;
    pool_opts.wait_timeout = pool_wait_timeout;
    return std::make_unique<Redis>(opts, pool_opts);
}

std::unique_ptr<Redis> make_sentinel_client(const std::string& master_name,
                                            const std::vector<std::pair<std::string, int>>& sentinels,
                                            size_t pool_size,
                                            const std::string& password,
                                            const std::string& sentinel_password,
                                            std::chrono::milliseconds socket_timeout,
                                            std::chrono::milliseconds pool_wait_timeout,
                                            int db) {
    const std::string effective_sentinel_pw = sentinel_password.empty() ? password : sentinel_password;
    SentinelOptions sentinel_opts;
    for (const auto& [host, port] : sentinels) {
        sentinel_opts.nodes.push_back({host, port});
    }
    sentinel_opts.connect_timeout = 200ms;
    sentinel_opts.socket_timeout = 200ms;
    if (!effective_sentinel_pw.empty())
        sentinel_opts.password = effective_sentinel_pw;

    ConnectionOptions conn_opts;
    conn_opts.connect_timeout = 200ms;
    conn_opts.socket_timeout = socket_timeout;
    conn_opts.db = db;
    if (!password.empty())
        conn_opts.password = password;

    ConnectionPoolOptions pool_opts;
    pool_opts.size = pool_size;
    pool_opts.wait_timeout = pool_wait_timeout;

    auto sentinel = std::make_shared<Sentinel>(sentinel_opts);
    return std::make_unique<Redis>(sentinel, master_name, Role::MASTER, conn_opts, pool_opts);
}

void CacheManager::initialize(const std::string& connection_str,
                              size_t pool_size,
                              const std::string& password,
                              std::chrono::milliseconds socket_timeout,
                              std::chrono::milliseconds pool_wait_timeout,
                              int db) {
    if (initialized) {
        throw std::runtime_error("Cache already initialized");
    }
    try {
        const RedisAddress addr = parse_redis_url(connection_str);
        redis_client =
            make_standalone_client(addr.host, addr.port, pool_size, password, socket_timeout, pool_wait_timeout, db);
        redis_client->ping();
        initialized = true;
        spdlog::info("Redis cache initialized (standalone: {}:{}, db={})", addr.host, addr.port, db);
    } catch (const Error& e) {
        spdlog::error("Failed to initialize Redis cache: {}", e.what());
        throw std::runtime_error("Redis initialization failed: " + std::string(e.what()));
    }
}

void CacheManager::initialize_with_sentinel(const std::string& master_name,
                                            const std::vector<std::pair<std::string, int>>& sentinels,
                                            size_t pool_size,
                                            const std::string& password,
                                            const std::string& sentinel_password,
                                            std::chrono::milliseconds socket_timeout,
                                            std::chrono::milliseconds pool_wait_timeout,
                                            int db) {
    if (initialized) {
        throw std::runtime_error("Cache already initialized");
    }
    try {
        redis_client = make_sentinel_client(
            master_name, sentinels, pool_size, password, sentinel_password, socket_timeout, pool_wait_timeout, db);
        redis_client->ping();
        initialized = true;
        spdlog::info("Redis cache initialized with Sentinel (master: {}, db={})", master_name, db);
    } catch (const Error& e) {
        spdlog::error("Failed to initialize Redis with Sentinel: {}", e.what());
        throw std::runtime_error("Redis Sentinel initialization failed: " + std::string(e.what()));
    }
}

bool CacheManager::set(const std::string& key, const std::string& value, long ttl) {
    return guarded_(
        false,
        [&] {
            if (ttl > 0) {
                redis_client->setex(key, ttl, value);
            } else {
                redis_client->set(key, value);
            }
            return true;
        },
        "Failed to set key '{}': {}",
        key);
}

bool CacheManager::set_nx(const std::string& key, const std::string& value, std::chrono::milliseconds ttl) {
    return guarded_(
        false,
        [&] { return redis_client->set(key, value, ttl, sw::redis::UpdateType::NOT_EXIST); },
        "Failed to SET NX key '{}': {}",
        key);
}

std::optional<std::string> CacheManager::get(const std::string& key) {
    return guarded_(
        std::optional<std::string>{},
        [&]() -> std::optional<std::string> {
            auto val = redis_client->get(key);
            if (val) {
                return *val;
            }
            return std::nullopt;
        },
        "Failed to get key '{}': {}",
        key);
}

long CacheManager::del(const std::string& key) {
    return guarded_(
        0L, [&] { return redis_client->del(key); }, "Failed to delete key '{}': {}", key);
}

long CacheManager::del(const std::vector<std::string>& keys) {
    return guarded_(
        0L, [&] { return redis_client->del(keys.begin(), keys.end()); }, "Failed to delete multiple keys: {}");
}

bool CacheManager::exists(const std::string& key) {
    return guarded_(
        false, [&] { return redis_client->exists(key) > 0; }, "Failed to check existence of key '{}': {}", key);
}

bool CacheManager::expire(const std::string& key, long seconds) {
    return guarded_(
        false, [&] { return redis_client->expire(key, seconds); }, "Failed to set expiration on key '{}': {}", key);
}

long CacheManager::ttl(const std::string& key) {
    return guarded_(
        -2L, [&] { return redis_client->ttl(key); }, "Failed to get TTL for key '{}': {}", key);
}

long long CacheManager::incr(const std::string& key, long long increment) {
    check_initialized();
    try {
        return redis_client->incrby(key, increment);
    } catch (const Error& e) {
        spdlog::error("Failed to increment key '{}': {}", key, e.what());
        throw;
    }
}

long long CacheManager::decr(const std::string& key, long long decrement) {
    check_initialized();
    try {
        return redis_client->decrby(key, decrement);
    } catch (const Error& e) {
        spdlog::error("Failed to decrement key '{}': {}", key, e.what());
        throw;
    }
}

long CacheManager::sadd(const std::string& key, const std::string& member) {
    return guarded_(
        0L, [&] { return redis_client->sadd(key, member); }, "Failed to add to set '{}': {}", key);
}

std::vector<std::string> CacheManager::smembers(const std::string& key) {
    return guarded_(
        std::vector<std::string>{},
        [&] {
            std::vector<std::string> members;
            redis_client->smembers(key, std::back_inserter(members));
            return members;
        },
        "Failed to get members of set '{}': {}",
        key);
}

long CacheManager::zadd(const std::string& key, const std::string& member, double score) {
    return guarded_(
        0L, [&] { return redis_client->zadd(key, member, score); }, "Failed to add to sorted set '{}': {}", key);
}

long long CacheManager::publish(const std::string& channel, const std::string& message) {
    return guarded_(
        0LL, [&] { return redis_client->publish(channel, message); }, "Failed to publish to channel '{}': {}", channel);
}

bool CacheManager::health_check() {
    if (!initialized)
        return false;
    try {
        redis_client->ping();
        return true;
    } catch (const Error& e) {
        spdlog::error("Cache health check failed: {}", e.what());
        return false;
    }
}

void CacheManager::shutdown() {
    if (initialized) {
        spdlog::info("Shutting down cache manager");
        redis_client.reset();
        initialized = false;
    }
}

Redis& CacheManager::get_client() {
    check_initialized();
    return *redis_client;
}

void CacheManager::check_initialized() const {
    if (!initialized) {
        throw std::runtime_error("Cache not initialized");
    }
}

namespace {

/// Global cache instance. File-local on purpose: everything outside this TU
/// reaches it through initialize() / get() / is_initialized() / shutdown()
/// and the test seam below (nothing else ever referenced it directly when it
/// was an inline header variable).
std::unique_ptr<CacheManager> global_cache = nullptr;

}  // namespace

void initialize(const std::string& connection_str,
                size_t pool_size,
                const std::string& password,
                std::chrono::milliseconds socket_timeout,
                std::chrono::milliseconds pool_wait_timeout,
                int db) {
    if (global_cache != nullptr) {
        throw std::runtime_error("Cache already initialized");
    }
    global_cache = std::make_unique<CacheManager>();
    global_cache->initialize(connection_str, pool_size, password, socket_timeout, pool_wait_timeout, db);
}

void initialize_with_sentinel(const std::string& master_name,
                              const std::vector<std::pair<std::string, int>>& sentinels,
                              size_t pool_size,
                              const std::string& password,
                              const std::string& sentinel_password,
                              std::chrono::milliseconds socket_timeout,
                              std::chrono::milliseconds pool_wait_timeout,
                              int db) {
    if (global_cache != nullptr) {
        throw std::runtime_error("Cache already initialized");
    }
    global_cache = std::make_unique<CacheManager>();
    global_cache->initialize_with_sentinel(
        master_name, sentinels, pool_size, password, sentinel_password, socket_timeout, pool_wait_timeout, db);
}

CacheManager& get() {
    if (global_cache == nullptr) {
        throw std::runtime_error("Cache not initialized");
    }
    return *global_cache;
}

bool is_initialized() {
    return global_cache != nullptr && global_cache->is_initialized();
}

void shutdown() {
    if (global_cache) {
        global_cache->shutdown();
        global_cache.reset();
    }
}

void install_for_testing(std::unique_ptr<CacheManager> fake) {
    global_cache = std::move(fake);
}

void reset_for_testing() {
    global_cache.reset();
}

}  // namespace Cache
