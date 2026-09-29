/**
 * @file Validation.hpp
 * @brief Composable request-body validators.
 * @details Controllers accumulate field errors into a @c Validation::Errors
 *          collector and emit a single 400 response with a structured error
 *          list. Keeps validation logic out of each controller's imperative
 *          tree of `if (…) return Response::ok(err, 400);`.
 *
 *          Response shape:
 *          @code
 *          {
 *            "error": "validation_failed",
 *            "errors": [
 *              {"field": "email", "code": "missing", "message": "required"},
 *              {"field": "username", "code": "too_short", "message": "min 1"}
 *            ]
 *          }
 *          @endcode
 */

#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <nlohmann/json.hpp>

#include "api/RequestUtils.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api::Validation {

using json = nlohmann::json;

// Password length bounds — single source for register / reset-password /
// change-password / admin-create. Mirrored by the zod schema on the frontend
// (frontend/src/lib/schemas/auth.ts).
inline constexpr std::size_t kPasswordMinLen = 8;
inline constexpr std::size_t kPasswordMaxLen = 128;

struct Error {
    std::string field;
    std::string code;
    std::string message;
};

/**
 * @brief Accumulator that each validator appends to.
 */
class Errors {
public:
    void add(std::string field, std::string code, std::string message) {
        items_.push_back({std::move(field), std::move(code), std::move(message)});
    }
    bool any() const { return !items_.empty(); }
    const std::vector<Error>& items() const { return items_; }

    // Build the `errors` array only — the full response body is assembled by
    // response_400() using the shared ErrorResponse shape.
    json errors_json() const {
        json arr = json::array();
        for (const auto& e : items_) {
            arr.push_back({{"field", e.field}, {"code", e.code}, {"message", e.message}});
        }
        return arr;
    }

private:
    std::vector<Error> items_;
};

// ---------------------------------------------------------------------------
// Field validators — each inspects body[field] and appends on failure.
// Every validator is a no-op on a field that doesn't exist (use require()
// first to enforce presence).
// ---------------------------------------------------------------------------

namespace detail {

/// Shared preamble of the string validators: absent/null → nullptr silently
/// (presence is require()'s job); non-string → nullptr after recording the
/// canonical "not_string" error. Otherwise a pointer to the string value.
inline const std::string* as_string(Errors& errs, const json& body, const std::string& field) {
    const auto it = body.find(field);
    if (it == body.end() || it->is_null())
        return nullptr;
    if (!it->is_string()) {
        errs.add(field, "not_string", "must be a string");
        return nullptr;
    }
    return &it->get_ref<const std::string&>();
}

}  // namespace detail

/**
 * @brief Require a field to be present AND a non-null value.
 */
inline bool require(Errors& errs, const json& body, const std::string& field) {
    if (!body.contains(field) || body[field].is_null()) {
        errs.add(field, "missing", "required");
        return false;
    }
    return true;
}

/**
 * @brief Require a field to be present, non-null AND a JSON string.
 * @details `require()` alone only proves presence, so a wrong-typed field
 *          (`{"password": 123}`) sails through and the handler's bare
 *          `body[f].get<std::string>()` throws nlohmann's type_error.302 —
 *          which escapes as a bare 500 instead of the 400 envelope, on
 *          unauthenticated paths included. Use this wherever the value is
 *          read as a string without a length/format validator downstream.
 */
inline bool require_string(Errors& errs, const json& body, const std::string& field) {
    if (!require(errs, body, field))
        return false;
    if (!body[field].is_string()) {
        errs.add(field, "not_string", "must be a string");
        return false;
    }
    return true;
}

/**
 * @brief Require a string field within [min, max] length. Non-strings fail
 *        with code "not_string". Missing fields are no-op (pair with require).
 */
inline void string_length(Errors& errs, const json& body, const std::string& field, size_t min_len, size_t max_len) {
    const auto* s = detail::as_string(errs, body, field);
    if (!s)
        return;
    if (s->size() < min_len) {
        errs.add(field, "too_short", "min length " + std::to_string(min_len));
    } else if (s->size() > max_len) {
        errs.add(field, "too_long", "max length " + std::to_string(max_len));
    }
}

/**
 * @brief Require a string field to match the given regex.
 */
inline void regex_match(
    Errors& errs, const json& body, const std::string& field, const std::regex& re, const std::string& format_hint) {
    const auto* s = detail::as_string(errs, body, field);
    if (!s)
        return;
    if (!std::regex_match(*s, re)) {
        errs.add(field, "bad_format", "expected format: " + format_hint);
    }
}

/**
 * @brief Require an integer field within [min, max].
 */
inline void int_range(Errors& errs, const json& body, const std::string& field, long long min_val, long long max_val) {
    if (!body.contains(field) || body[field].is_null())
        return;
    if (!body[field].is_number_integer()) {
        errs.add(field, "not_integer", "must be an integer");
        return;
    }
    auto v = body[field].get<long long>();
    if (v < min_val) {
        errs.add(field, "below_min", "min " + std::to_string(min_val));
    } else if (v > max_val) {
        errs.add(field, "above_max", "max " + std::to_string(max_val));
    }
}

/**
 * @brief Require a boolean field when present. Missing/null is a no-op.
 *        Pair with a `contains() && is_boolean()` read — `body.value(f, false)`
 *        throws type_error.302 on a non-boolean (and on an explicit null).
 */
inline void boolean(Errors& errs, const json& body, const std::string& field) {
    if (!body.contains(field) || body[field].is_null())
        return;
    if (!body[field].is_boolean())
        errs.add(field, "not_boolean", "must be a boolean");
}

/**
 * @brief Require a string field to be one of a fixed set of values.
 */
inline void one_of(Errors& errs, const json& body, const std::string& field, const std::vector<std::string>& allowed) {
    const auto* s = detail::as_string(errs, body, field);
    if (!s)
        return;
    for (const auto& a : allowed)
        if (*s == a)
            return;
    std::string msg = "must be one of:";
    for (const auto& a : allowed) {
        msg += " '" + a + "'";
    }
    errs.add(field, "not_allowed", std::move(msg));
}

/**
 * @brief Pull an optional string field. Returns nullopt when the field is
 *        absent or not a string — collapses the
 *        `if (body.contains(f) && body[f].is_string()) x = body[f]...`
 *        boilerplate that recurs across the auth/admin controllers.
 */
inline std::optional<std::string> opt_string(const json& body, const std::string& field) {
    if (body.contains(field) && body[field].is_string())
        return body[field].get<std::string>();
    return std::nullopt;
}

inline void email(Errors& errs, const json& body, const std::string& field) {
    // Pragmatic email regex — not a spec-compliant RFC 5322 parser, but
    // rejects the obvious bad inputs without blocking valid ones.
    static const std::regex re(R"(^[A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,}$)");
    regex_match(errs, body, field, re, "email");
}

inline void uuid(Errors& errs, const json& body, const std::string& field) {
    // Single source of truth for the 8-4-4-4-12 hex shape: the char-walk in
    // RequestUtils (Api::detail::is_uuid_segment) — this used to be a second,
    // regex-based UUID implementation that could drift from it. Same error
    // contract as the regex validators: "bad_format" on mismatch, "not_string"
    // on a wrong-typed field, no-op when absent (presence is require()'s job).
    const auto* s = detail::as_string(errs, body, field);
    if (!s)
        return;
    if (!Api::detail::is_uuid_segment(*s))
        errs.add(field, "bad_format", "expected format: uuid");
}

// ---------------------------------------------------------------------------
// Response helper
// ---------------------------------------------------------------------------

inline drogon::HttpResponsePtr response_400(const Errors& errs) {
    return ErrorResponse::bad_request("validation_failed", "", json{{"errors", errs.errors_json()}});
}

/**
 * @brief Parse the request body as JSON. Returns true on success and fills
 *        @p out; on failure invokes @p cb with a 400 response and returns false.
 *        Eliminates the 5-line try/catch boilerplate in every mutating handler.
 *
 * Usage:
 * @code
 *   json body;
 *   if (!Validation::parse_body(req, body, callback)) return;
 * @endcode
 */
inline bool parse_body(const drogon::HttpRequestPtr& req,
                       json& out,
                       std::function<void(const drogon::HttpResponsePtr&)>& cb) {
    try {
        out = json::parse(req->body());
        return true;
    } catch (...) {
        cb(ErrorResponse::bad_request("invalid_json", "Invalid JSON body"));
        return false;
    }
}

}  // namespace Api::Validation
