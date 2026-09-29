/**
 * @file PathNormalize.hpp
 * @brief Request-path normalization + UUID-shape validation — the drogon-free
 *        half of RequestUtils.hpp.
 * @details Split out so the fuzz_path_match libFuzzer harness (tests/fuzz)
 *          can include it without <drogon/HttpRequest.h> (and therefore
 *          without the vcpkg dependency world). RequestUtils.hpp includes
 *          this header, so existing includers see the exact same names in
 *          the exact same namespaces.
 */

#pragma once

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace Api {

namespace detail {

/// True if @p s is a canonical 8-4-4-4-12 lowercase/uppercase-hex UUID.
inline bool is_uuid_segment(std::string_view s) {
    if (s.size() != 36)
        return false;
    for (size_t i = 0; i < 36; ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-')
                return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

}  // namespace detail

/**
 * @brief Validate UUID format (8-4-4-4-12 hex)
 */
inline bool is_valid_uuid(const std::string& str) {
    return detail::is_uuid_segment(str);
}

/**
 * @brief Normalize a request path for metric/trace cardinality AND log
 *        redaction. Replaces UUID segments with ":id" and the single-use
 *        token after the account confirm/reset/change-email routes with
 *        ":token".
 * @details Two jobs in one: (a) raw ids/tokens would mint a new Prometheus
 *          label and Jaeger operation per entity (cardinality blow-up);
 *          (b) the account tokens are credentials — logging the raw path
 *          would drop password-reset tokens into the access log. A manual
 *          segment scan (no std::regex) keeps this cheap on the hot path.
 */
inline std::string normalize_path_for_metrics(const std::string& path) {
    // Split into segments, rewrite, rejoin. Empty input / "/" returns as-is.
    std::vector<std::string_view> segs;
    size_t i = 0;
    while (i < path.size()) {
        if (path[i] == '/') {
            ++i;
            continue;
        }
        size_t j = path.find('/', i);
        if (j == std::string::npos)
            j = path.size();
        segs.emplace_back(path.data() + i, j - i);
        i = j;
    }

    // Optional API version segment (/api/v<N>/... — see ADR 0006), so the token
    // routes are detected whether or not a version is present.
    auto is_version_seg = [](const auto& s) {
        if (s.size() < 2 || s[0] != 'v')
            return false;
        for (size_t k = 1; k < s.size(); ++k)
            if (s[k] < '0' || s[k] > '9')
                return false;
        return true;
    };
    const size_t base = (segs.size() >= 2 && is_version_seg(segs[1])) ? 2 : 1;  // index of <resource> after /api[/vN]

    // The account token routes: /api[/vN]/account/<verb>/<token> where verb is one
    // of the token-bearing apply endpoints (the *-request / *-resend variants
    // are single segments and won't match this shape).
    const size_t token_idx = base + 2;
    const bool account_token_route = segs.size() == base + 3 && segs[0] == "api" && segs[base] == "account" &&
                                     (segs[base + 1] == "confirm" || segs[base + 1] == "reset-password" ||
                                      segs[base + 1] == "change-email" || segs[base + 1] == "join-from-invite");

    std::string out;
    out.reserve(path.size());
    for (size_t k = 0; k < segs.size(); ++k) {
        out += '/';
        if (account_token_route && k == token_idx) {
            out += ":token";
            continue;
        }
        // Bucket id-shaped segments so per-id paths don't explode metric
        // cardinality: uuids (e.g. /api/admin/users/<uuid>) AND all-digit ids
        // (e.g. /api/admin/roles/5 — integer PKs added with the roles routes).
        bool all_digits = !segs[k].empty();
        for (char c : segs[k])
            if (c < '0' || c > '9') {
                all_digits = false;
                break;
            }
        if (all_digits || detail::is_uuid_segment(segs[k]))
            out += ":id";
        else
            out.append(segs[k].data(), segs[k].size());
    }
    if (out.empty())
        out = "/";
    return out;
}

}  // namespace Api
