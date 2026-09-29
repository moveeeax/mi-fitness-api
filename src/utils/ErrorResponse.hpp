/**
 * @file ErrorResponse.hpp
 * @brief Unified JSON error body used by every controller and middleware.
 * @details One stable shape everywhere:
 * @code
 *   {
 *     "error":   "<machine-readable code>",
 *     "status":  <http status int>,
 *     "message": "<optional human message>",
 *     // extra fields merged in at top level, e.g.:
 *     "errors":          [...],    // validation
 *     "retry_after_sec": N,        // rate limit
 *     "code":            "..."     // auth
 *   }
 * @endcode
 *
 * Avoid building HttpResponse + JSON body ad-hoc in controllers — use the
 * helpers below so the frontend always parses the same shape.
 *
 * Declarations only — the bodies live in ErrorResponse.cpp (compiled once
 * into app_core; ADR 0003 as amended 2026-08-22). The drogon include stays:
 * every builder RETURNS a drogon::HttpResponsePtr and Error carries a
 * drogon::HttpStatusCode — the header cannot shed the drogon dependency
 * without changing this API's types, which is out of scope here.
 */

#pragma once

#include <string>

#include <drogon/HttpResponse.h>

#include <nlohmann/json.hpp>

namespace ErrorResponse {

using json = nlohmann::json;

struct Error {
    drogon::HttpStatusCode status;
    std::string code;             // stable machine code: "not_found", "validation_failed", ...
    std::string message;          // optional human-readable detail
    json extra = json::object();  // optional extra keys merged at top level
};

drogon::HttpResponsePtr make(Error e);

// ---- shorthand builders ---------------------------------------------------

drogon::HttpResponsePtr bad_request(std::string code, std::string message = "", json extra = json::object());

// The WWW-Authenticate challenge header is added by the auth middleware
// (Middleware.cpp::register_auth), which has the scheme/error context — keep
// the body builder pure here.
drogon::HttpResponsePtr unauthorized(std::string code = "unauthorized", std::string message = "");

drogon::HttpResponsePtr forbidden(std::string code = "forbidden", std::string message = "");

drogon::HttpResponsePtr not_found(std::string what = "resource");

drogon::HttpResponsePtr conflict(std::string code, std::string message = "");

drogon::HttpResponsePtr payload_too_large(std::string code = "payload_too_large");

drogon::HttpResponsePtr unsupported_media_type(std::string code = "unsupported_media_type", std::string message = "");

drogon::HttpResponsePtr unprocessable(std::string code, std::string message = "");

drogon::HttpResponsePtr too_many_requests(int retry_after_sec);

drogon::HttpResponsePtr internal_error(std::string code = "internal_error", std::string message = "");

drogon::HttpResponsePtr service_unavailable(std::string code = "service_unavailable", std::string message = "");

}  // namespace ErrorResponse

namespace Response {

using json = nlohmann::json;

/**
 * @brief Build a 200 application/json response from a JSON body.
 *        Centralized so controllers don't repeat newHttpResponse + setBody +
 *        setContentTypeCode + setStatusCode in every handler.
 */
drogon::HttpResponsePtr ok(const json& body);

/// 201 Created variant.
drogon::HttpResponsePtr created(const json& body);

/**
 * @brief 200 with the standard paginated list envelope: one shape every list
 *        endpoint should emit so clients can rely on it. @p data must be a JSON
 *        array. Pairs with RequestUtils::parse_page_params.
 */
drogon::HttpResponsePtr paginated(const json& data, long total, int limit, int offset);

/// 200 with a bare collection envelope: {data:[...]} (unpaginated lists).
drogon::HttpResponsePtr list(const json& data);

}  // namespace Response
