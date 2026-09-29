/**
 * @file RequestUtils.hpp
 * @brief Small request-parsing helpers shared by controllers and middleware.
 * @details Everything here is `inline` with external linkage — these used to
 *          live in an anonymous namespace inside Api.hpp, which is an ODR
 *          trap for inline callers (internal-linkage entities referenced from
 *          inline functions make the definitions differ across TUs).
 *
 *          The drogon-free half (is_valid_uuid, normalize_path_for_metrics)
 *          lives in PathNormalize.hpp, re-exported here — the
 *          fuzz_path_match harness (tests/fuzz) includes that header
 *          without pulling <drogon/HttpRequest.h>.
 */

#pragma once

#include <string>

#include <drogon/HttpRequest.h>

#include "api/PathNormalize.hpp"

namespace Api {

/**
 * @brief Parse a query-param string to int, returning @p def on empty/invalid.
 */
inline int parse_int(const std::string& s, int def) {
    if (s.empty())
        return def;
    try {
        return std::stoi(s);
    } catch (...) {
        return def;
    }
}

/**
 * @brief parse_int + clamp into [lo, hi]. Shared by list endpoints that take
 *        a `limit`/`offset` query param.
 */
inline int clamp_int(const std::string& s, int def, int lo, int hi) {
    int v = parse_int(s, def);
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

/**
 * @brief Parsed limit/offset pair for offset-paginated list endpoints.
 *        One semantics everywhere: out-of-range values CLAMP into the
 *        documented bounds (they don't silently reset to the default).
 */
struct PageParams {
    int limit = 20;
    int offset = 0;
};

inline PageParams parse_page_params(const drogon::HttpRequestPtr& req, int default_limit, int max_limit) {
    PageParams p;
    p.limit = clamp_int(req->getParameter("limit"), default_limit, 1, max_limit);
    p.offset = clamp_int(req->getParameter("offset"), 0, 0, 1000000);
    return p;
}

}  // namespace Api
