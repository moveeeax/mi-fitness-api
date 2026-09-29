/**
 * @file Idempotency.hpp
 * @brief Idempotency-Key middleware for mutating HTTP requests.
 * @details When a client sends `Idempotency-Key: <key>` on POST/PUT/PATCH/DELETE,
 *          the first request's response is stored in Redis keyed by
 *          (method, path, key). A subsequent request with the same key:
 *          - replays the cached response if the body hash matches;
 *          - returns 422 if the body hash differs (signals client bug);
 *          If Redis is unavailable we fail open (request proceeds normally) —
 *          the alternative (reject all mutating requests when cache is down)
 *          is worse than a rare double-processing.
 *
 * Declarations only for the non-template bodies — they live in
 * Idempotency.cpp (compiled once into app_core; ADR 0003 as amended
 * 2026-08-22). The Config struct and the attribute keys below stay here.
 */

#pragma once

#include <cstddef>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

namespace Security::Idempotency {

struct Config {
    bool enabled = false;
    long ttl_sec = 86400;                    // 24h
    size_t max_body_bytes = 1024 * 1024;     // 1 MiB — reject larger REQUEST bodies
    size_t max_response_bytes = 256 * 1024;  // 256 KiB — skip caching oversized responses
    // Lock TTL for the in-flight marker (SET NX). Bounds how long a crashed or
    // stuck handler can block a retry from another client with the same key.
    // Should comfortably exceed p99 handler latency.
    int lock_ttl_sec = 30;
};

void initialize();

bool is_initialized();

const Config& config();

void shutdown();

// ---------------------------------------------------------------------------
// Middleware attribute keys (shared between pre- and post-handlers).
// ---------------------------------------------------------------------------

inline constexpr const char* kKeyAttr = "_idemp_cache_key";
inline constexpr const char* kHashAttr = "_idemp_body_hash";
inline constexpr const char* kLockKeyAttr = "_idemp_lock_key";

/**
 * @brief Pre-handling check. Returns a response to short-circuit the handler
 *        (either a replay of a prior response, or a 422 on conflict), or
 *        empty to let the request proceed with the key stashed in attributes.
 */
drogon::HttpResponsePtr pre_handle(const drogon::HttpRequestPtr& req);

/**
 * @brief Post-handling hook. Stores the response (body + status) in Redis
 *        so a future request with the same Idempotency-Key can replay it.
 *        Only 2xx responses are cached — errors may be transient.
 */
void post_handle(const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& resp);

}  // namespace Security::Idempotency
