/**
 * @file RateLimit.hpp
 * @brief Redis-backed SLIDING-window request rate limiter.
 * @details One atomic Lua script per check: ZREMRANGEBYSCORE + ZCARD + ZADD
 *          over a per-identity sorted set — a true trailing window, immune to
 *          the classic 2x burst at fixed-window boundaries.
 *          Fail-open: if Redis is unavailable, requests pass through with a
 *          warning log — we prefer availability over strict enforcement for a
 *          template. Production deployments that need hard caps should set
 *          fail_open=false (rejects with 503 instead).
 *
 * Declarations only for the non-template bodies — they live in RateLimit.cpp
 * (compiled once into app_core; ADR 0003 as amended 2026-08-22). The
 * Config/Decision structs and the Limiter class shape stay here.
 */

#pragma once

#include <string>
#include <unordered_set>
#include <utility>

#include <drogon/HttpRequest.h>

namespace Security::RateLimit {

// Scope controls how a request is bucketed:
//   Ip        — always by client IP.
//   IpOrUser  — by authenticated user if one is present, else by IP.
// An earlier draft exposed a third "user-only" scope, but in practice that
// lumped every anonymous caller into a single bucket — hostile for any route
// that allows unauthenticated traffic. We fold it into IpOrUser.
enum class Scope { Ip, IpOrUser };

Scope parse_scope(const std::string& s);

struct Config {
    bool enabled = false;
    int requests = 60;    // max per window
    int window_sec = 60;  // window size
    // Stricter tier for the public auth/account surface (login, register,
    // refresh, password-reset, token links). Those paths are auth-public, so
    // the general limiter's public_paths skip leaves them unthrottled — this
    // tier re-arms them with a tight per-IP cap.
    int protected_requests = 10;
    int protected_window_sec = 60;
    Scope scope = Scope::IpOrUser;
    bool trust_proxy = false;                   // read X-Forwarded-For
    int trusted_proxy_count = 1;                // # of trusted hops appended to XFF (index from the right)
    bool fail_open = true;                      // allow on Redis error
    std::unordered_set<std::string> whitelist;  // IPs or user subjects
    std::unordered_set<std::string> public_paths;
    std::unordered_set<std::string> protected_paths;  // auth surface, limited despite being public
};

struct Decision {
    bool allowed = true;
    int remaining = 0;
    int retry_after_sec = 0;
};

class Limiter {
public:
    explicit Limiter(Config cfg) : cfg_(std::move(cfg)) {}

    const Config& config() const { return cfg_; }

    // General tier: skips api.public_paths, buckets by ip_or_user.
    Decision check(const std::string& identity) const;

    // Stricter tier for the public auth/account surface (login, register,
    // refresh, password-reset, token links). Separate Redis key namespace
    // ("rl:auth:") so it counts independently from the general limiter — a
    // burst of logins doesn't consume a user's normal API budget and vice
    // versa.
    Decision check_protected(const std::string& identity) const;

private:
    Decision check_window(const std::string& identity, int requests, int window_sec, const char* key_prefix) const;

    Decision fallback_(const std::string& reason, int requests) const;

    Config cfg_;
};

// ---------------------------------------------------------------------------
// Global singleton
// ---------------------------------------------------------------------------

void initialize();

bool is_initialized();

Limiter& get();

void shutdown();

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------

std::string client_ip(const drogon::HttpRequestPtr& req, bool trust_proxy, int trusted_proxy_count = 1);

// Convenience: resolve trust_proxy / trusted_proxy_count straight from app
// config, so any caller (audit, request logging, …) gets the SAME trusted
// client-IP logic as the limiter — even when rate limiting itself is disabled
// (the limiter may not be initialized, but the config keys still apply). With
// trust_proxy=false this returns peerAddr (the proxy hop) rather than a
// spoofable, client-supplied X-Real-IP header.
std::string client_ip(const drogon::HttpRequestPtr& req);

std::string identity_for(const drogon::HttpRequestPtr& req, const Config& cfg);

// Always-by-IP identity for the protected tier. Login/register carry no
// authenticated principal, and we don't want a (possibly attacker-supplied,
// expired) cookie's subject to shift the bucket — the brute-force unit is the
// source IP regardless of the configured scope.
std::string ip_identity(const drogon::HttpRequestPtr& req, const Config& cfg);

}  // namespace Security::RateLimit
