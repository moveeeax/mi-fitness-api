/**
 * @file Middleware.hpp
 * @brief Drogon advice chain: content-type check, auth, rate limit,
 *        idempotency, CORS, tracing, access log + HTTP metrics, and the
 *        optional Swagger UI endpoints.
 * @details Registration order matters and is owned by
 *          Api::register_controllers() in Api.hpp.
 *
 * Declarations only — the bodies live in Middleware.cpp (compiled once into
 * app_core; ADR 0003 as amended 2026-08-22): including this header no longer
 * pulls the OTel SDK, spdlog or the security module headers
 * (Auth/RateLimit/Idempotency/Csrf/ApiKeys) into the including TU. Only the
 * drogon request/response types survive in the signatures.
 */

#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

namespace Api {

namespace middleware {

/**
 * @brief Lazily create the HTTP metric families (http_requests_total,
 *        http_request_duration_seconds) in the Observability registry.
 *        No-op when Observability is down; the family pointers live in
 *        Middleware.cpp.
 */
void ensure_http_metric_families();

/**
 * @brief Terminate a request that a sync advice is short-circuiting.
 * @details Drogon's HttpServer::passSyncAdvices() writes the response and
 *          returns false, so NOTHING else runs for it — no pre-handling
 *          advice, and no post-handling chain: no X-Request-Id, no
 *          traceparent, no baseline security headers, no access-log line, no
 *          metric sample. Every sync advice therefore returns THROUGH this
 *          helper, which replays what the post-handling chain would have done.
 */
drogon::HttpResponsePtr short_circuit(const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& resp);

/**
 * @brief First sync advice in the chain: mint the request/trace ids.
 * @details Never returns a response — it exists purely so that the advices
 *          registered after it (content-type, auth, csrf, rate limit,
 *          idempotency, cors) have ids to stamp on a short-circuited response.
 */
void register_request_id();

void register_auth();

void register_rate_limit();

/**
 * @brief Double-submit-cookie CSRF guard (opt-in via security.csrf.enabled).
 * @details Enforces, for cookie-authenticated state-changing requests, that the
 *          CSRF cookie value is echoed in the configured header. The decision
 *          lives in Security::Csrf::passes() (unit-tested); this advice just
 *          feeds it the request's method/cookies/header. Off by default — the
 *          token cookie is emitted by set_session_cookies only when enabled.
 */
void register_csrf();

void register_idempotency();

/**
 * @brief Middleware that rejects POST/PUT/PATCH with non-JSON Content-Type.
 * @details Without this guard, json::parse(body) inside controllers throws on
 *          form-encoded or text bodies and surfaces as a 500. The spec answer
 *          is 415 Unsupported Media Type — easier to debug from the client
 *          side. Empty body (no Content-Type at all) is allowed: not every
 *          mutation carries a payload.
 *
 *          Recognized types: application/json plus any "+json" suffixed
 *          subtype (charset parameters are stripped before comparison), and
 *          multipart/form-data for the upload surface.
 */
void register_content_type_check();

void register_cors();

/**
 * @brief Stamp baseline security headers on every response.
 * @details API responses are JSON, so the CSP is locked all the way down
 *          (default-src 'none') — nothing should ever execute or embed from an
 *          API origin. The SPA's own HTML/CSP is set at the edge (nginx). HSTS
 *          is opt-in (security.hsts): it's only honoured over HTTPS, but gating
 *          it keeps it out of plain-http dev. set_if_absent never clobbers a
 *          header a handler deliberately set.
 */
void register_security_headers();

void register_tracing_pre();

void register_access_log_post();

}  // namespace middleware

/**
 * @brief Register /api/v1/docs (Swagger UI) and /api/v1/openapi.yaml if
 *        `docs.enabled` is true. Off by default — intended for dev and
 *        internal deployments, never production. The Swagger UI HTML is
 *        served inline (tiny snippet pointing at the unpkg CDN), and the
 *        YAML is streamed from the path configured in `docs.openapi_path`.
 */
void register_docs_endpoints();

}  // namespace Api
