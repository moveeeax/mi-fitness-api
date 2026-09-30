/**
 * @file Strings.hpp
 * @brief Small string helpers used in multiple modules.
 *
 * Declarations only — the bodies live in Strings.cpp (compiled once into
 * app_core; ADR 0003 as amended 2026-08-22). The kDefault*Csv path constants
 * stay inline constexpr here: they are read directly by Auth / RateLimit /
 * BillingController and the tests, not just by the bodies below.
 */

#pragma once

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Utils::Strings {

/**
 * @brief Paths that every middleware treats as never-authenticated and
 *        never-rate-limited. Read once from `api.public_paths` config /
 *        `API_PUBLIC_PATHS` env and reused by all security modules to
 *        avoid skew between per-module overrides.
 *
 * Entries are exact-match, except a trailing `*` matches by prefix — needed
 * for the token-bearing account routes (confirm / reset / change-email),
 * which carry the token as a path segment and so can't be matched exactly.
 * Those flows MUST be reachable without a session (the user clicking an email
 * link isn't logged in), so they ship public by default. Note the static
 * `*-request` / `confirm-resend` routes are deliberately NOT here:
 * change-email-request and confirm-resend require an authenticated principal.
 *
 * `/api/v1/billing/paypal/webhook` is PayPal's own server calling us, not a
 * browser — there is no session to authenticate against, and the handler
 * (BillingController::paypalWebhook) verifies PayPal's own request signature
 * itself before trusting anything in the body. It ships in the default even
 * though billing is off by default: the controller 404s while the module is
 * disabled, and a deployment that flips BILLING_ENABLED=true must not also
 * have to discover an auth override. It MUST also live in
 * `config/config.json`'s `api.public_paths` — see the override semantics
 * right below.
 * Config semantics — two keys, two behaviors:
 *   - `api.public_paths` / `API_PUBLIC_PATHS` REPLACES this default wholesale.
 *     Forget one default entry in the override and that route starts 401ing —
 *     this bit both the content module (docs/CONFIG.md, Content section) and a
 *     downstream fork's PayPal webhook, which was added only to this constant
 *     and silently died under any deployment that set the override key.
 *   - `api.public_paths_extra` / `API_PUBLIC_PATHS_EXTRA` is ADDITIVE: its
 *     entries are appended to whichever base won above. To open ONE new route
 *     (a payment-provider webhook, a public feed), use the extra key — never
 *     re-list the world.
 * (Route globs are spelled without the star in this comment on purpose: a
 * slash-star pair inside a block comment trips -Wcomment, and CI is -Werror.)
 */
inline constexpr const char* kDefaultPublicPathsCsv =
    "/,/healthz,/ready,/health,/metrics,"
    "/api/v1/docs,/api/v1/openapi.yaml,"
    "/api/v1/auth/login,/api/v1/auth/refresh";

/**
 * @brief Public endpoints that must STILL be rate-limited despite being
 *        auth-public. These are the brute-force surfaces: login
 *        (credential stuffing) and refresh (token churn).
 *
 * This is kDefaultPublicPathsCsv minus the
 * infra and static surface (`/`, `/healthz`, `/ready`, `/health`, `/metrics`,
 * `/api/v1/docs`, `/api/v1/openapi.yaml`), which we never want to throttle. The
 * general limiter skips everything in api.public_paths; without this list the
 * auth and content surfaces would be skipped too, leaving them wide open.
 * Matched the same way as public paths (exact, or trailing `*` prefix).
 */
inline constexpr const char* kDefaultProtectedPathsCsv = "/api/v1/auth/login,/api/v1/auth/refresh";

/**
 * @brief True if @p path is covered by @p public_paths — exact match, or a
 *        prefix match for an entry ending in `*`. Shared by Auth / RateLimit /
 *        Idempotency so they can't disagree about what's public.
 */
bool path_is_public(const std::unordered_set<std::string>& public_paths, const std::string& path);

/// Canonical truthy set for boolean flags: "true" / "1" / "yes". Shared by
/// Config's env/bool parsing and the direct getenv checks so the two paths
/// can't drift on what counts as "on".
bool flag_true(std::string_view value);

/// True iff env var @p name is set to a truthy value (see flag_true).
bool env_flag_true(const char* name);

/// Join @p v with @p sep between elements ("a,b,c" for sep ",").
std::string join(const std::vector<std::string>& v, const char* sep);

/**
 * @brief Split @p csv on commas, dropping empty components.
 */
std::vector<std::string> split_csv_vec(const std::string& csv);

/// CSV → unordered_set wrapper for callers that need set semantics (auth /
/// rate-limit public paths). Built on top of split_csv_vec — single source.
std::unordered_set<std::string> split_csv_set(const std::string& csv);

/// Base CSV (full-override semantics) + extra CSV (additive semantics) → one
/// set. This is THE way `api.public_paths` and `api.public_paths_extra` are
/// combined — Auth and RateLimit both call it so the two middlewares can't
/// disagree about what the extra key means. Empty extra is a no-op.
std::unordered_set<std::string> merge_csv_sets(const std::string& base_csv, const std::string& extra_csv);

/// Partially redact an email for logs: keep the first local char and the full
/// domain, mask the rest ("john.doe@example.com" -> "j***@example.com"). A
/// single-char (or empty) local part is fully masked so it can't be recovered.
/// Inputs without '@' are treated as the local part (defensive — not a real
/// address). Use everywhere an address would otherwise land in a log line; raw
/// PII in logs is inherited by every fork by default.
std::string mask_email(const std::string& email);

}  // namespace Utils::Strings
