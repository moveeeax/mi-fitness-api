/**
 * @file Auth.cpp
 * @brief Bodies for src/security/Auth.hpp — compiled once into app_core: JWT
 *        claim validation, config loading, the global Authenticator lifecycle
 *        and the RBAC / permission helpers. The enums, structs, constants and
 *        the Authenticator class shape stay in the header; every contract is
 *        documented on the declarations there.
 */

#include "security/Auth.hpp"

#include <algorithm>
#include <memory>
#include <sstream>
#include <stdexcept>

#include <spdlog/spdlog.h>

#include "security/Jwt.hpp"
#include "utils/Config.hpp"
#include "utils/ErrorResponse.hpp"
#include "utils/Strings.hpp"
#include "utils/Time.hpp"

namespace Security::Auth {

namespace {

std::vector<std::string> extract_string_array(const json& node) {
    std::vector<std::string> out;
    if (node.is_array()) {
        for (const auto& v : node) {
            if (v.is_string())
                out.push_back(v.get<std::string>());
        }
    } else if (node.is_string()) {
        // Space-separated (OAuth2 scope convention).
        std::istringstream iss(node.get<std::string>());
        std::string tok;
        while (iss >> tok)
            out.push_back(tok);
    }
    return out;
}

}  // namespace

std::optional<AuthPrincipal> Authenticator::verify_jwt(const std::string& token, std::string& err) const {
    auto claims_opt = verify_hs256_jwt(token, config_.jwt_secret, err);
    if (!claims_opt)
        return std::nullopt;
    const auto& claims = *claims_opt;

    // A refresh token must never authenticate a request. It outlives the
    // access TTL (7d vs 15m) and is only revocation-checked on /refresh,
    // so accepting it here would bypass both. We reject typ=="refresh"
    // rather than require typ=="access" so typ-less third-party access
    // tokens (and make-jwt.sh) keep working.
    if (claims.value("typ", "") == "refresh") {
        err = "wrong_token_type";
        return std::nullopt;
    }

    const auto now = Utils::Time::now_epoch_seconds();
    const auto leeway = static_cast<int64_t>(config_.jwt_leeway_sec);

    if (claims.contains("exp") && claims["exp"].is_number_integer()) {
        auto exp = claims["exp"].get<int64_t>();
        if (now > exp + leeway) {
            err = "token_expired";
            return std::nullopt;
        }
    }
    if (claims.contains("nbf") && claims["nbf"].is_number_integer()) {
        auto nbf = claims["nbf"].get<int64_t>();
        if (now + leeway < nbf) {
            err = "token_not_yet_valid";
            return std::nullopt;
        }
    }
    if (!config_.jwt_issuer.empty() && claims.value("iss", "") != config_.jwt_issuer) {
        err = "bad_issuer";
        return std::nullopt;
    }
    if (!config_.jwt_audience.empty()) {
        bool aud_ok = false;
        if (claims.contains("aud")) {
            if (claims["aud"].is_string()) {
                aud_ok = (claims["aud"].get<std::string>() == config_.jwt_audience);
            } else if (claims["aud"].is_array()) {
                for (const auto& a : claims["aud"]) {
                    if (a.is_string() && a.get<std::string>() == config_.jwt_audience) {
                        aud_ok = true;
                        break;
                    }
                }
            }
        }
        if (!aud_ok) {
            err = "bad_audience";
            return std::nullopt;
        }
    }

    AuthPrincipal p;
    p.subject = claims.value("sub", "");
    if (claims.contains(config_.jwt_roles_claim)) {
        p.roles = extract_string_array(claims[config_.jwt_roles_claim]);
    }
    if (claims.contains(config_.jwt_scopes_claim)) {
        p.scopes = extract_string_array(claims[config_.jwt_scopes_claim]);
    }
    // Optional — most tokens have no "org" claim at all and that is fine
    // (see the field comment on AuthPrincipal::org); verify_jwt never
    // requires it, so pre-orgs tokens keep working unchanged.
    p.org = claims.value("org", "");
    // All claim reads above are done — safe to steal the parsed JSON
    // instead of deep-copying it into the principal.
    p.raw_claims = std::move(*claims_opt);
    return p;
}

bool Authenticator::path_is_public(const std::string& path) const {
    return Utils::Strings::path_is_public(config_.public_paths, path);
}

namespace {

/// Global Authenticator instance. File-local on purpose: everything outside
/// this TU reaches it through initialize() / is_initialized() / get() /
/// shutdown() / install_for_testing() below (nothing else ever referenced it
/// directly when it was an inline header variable).
std::unique_ptr<Authenticator> global_auth = nullptr;

AuthMode parse_mode(const std::string& s) {
    if (s == "jwt")
        return AuthMode::Jwt;
    if (s == "bearer")
        return AuthMode::Bearer;
    return AuthMode::None;
}

}  // namespace

AuthConfig load_config_from_global() {
    AuthConfig cfg;
    if (!Config::is_initialized())
        return cfg;
    auto& c = Config::get();

    std::string mode_str = c.get<std::string>("auth.mode", "AUTH_MODE", "none");
    cfg.mode = parse_mode(mode_str);

    cfg.bearer_token = c.get<std::string>("auth.bearer_token", "AUTH_BEARER_TOKEN", "");
    cfg.jwt_secret = c.get<std::string>("auth.jwt.secret", "JWT_SECRET", "");
    cfg.jwt_issuer = c.get<std::string>("auth.jwt.issuer", "JWT_ISSUER", "");
    cfg.jwt_audience = c.get<std::string>("auth.jwt.audience", "JWT_AUDIENCE", "");
    cfg.jwt_leeway_sec = c.get<int>("auth.jwt.leeway_sec", "JWT_LEEWAY_SEC", 30);
    cfg.jwt_roles_claim = c.get<std::string>("auth.jwt.roles_claim", "JWT_ROLES_CLAIM", "roles");
    cfg.jwt_scopes_claim = c.get<std::string>("auth.jwt.scopes_claim", "JWT_SCOPES_CLAIM", "scope");

    // Single source of truth for public paths across Auth + RateLimit +
    // Idempotency. Module-specific override keys intentionally NOT supported
    // to prevent drift between middlewares.
    //
    // api.public_paths is a FULL OVERRIDE of the built-in default;
    // api.public_paths_extra is ADDITIVE (appended to whichever base won).
    // The extra key exists because two deployments independently hit the same
    // trap: a route added only to kDefaultPublicPathsCsv (content module, a
    // fork's PayPal webhook) 401'd everywhere the override key was set.
    std::string paths_csv =
        c.get<std::string>("api.public_paths", "API_PUBLIC_PATHS", Utils::Strings::kDefaultPublicPathsCsv);
    std::string extra_csv = c.get<std::string>("api.public_paths_extra", "API_PUBLIC_PATHS_EXTRA", "");
    cfg.public_paths = Utils::Strings::merge_csv_sets(paths_csv, extra_csv);

    if (cfg.mode == AuthMode::Jwt && cfg.jwt_secret.empty()) {
        throw std::runtime_error("auth.mode=jwt but JWT_SECRET is empty — set auth.jwt.secret or JWT_SECRET env");
    }
    // Enforce a minimum secret length at boot (not just non-empty): a short
    // HS256 key is brute-forceable offline → token forgery → full admin. The
    // same master secret derives the email-link token keys (Tokens.hpp), so this
    // guards both surfaces. 32 bytes matches the HMAC-SHA256 output size and the
    // prod-check.sh threshold (which is opt-in and didn't cover this path).
    if (cfg.mode == AuthMode::Jwt && cfg.jwt_secret.size() < 32) {
        throw std::runtime_error("auth.mode=jwt but JWT_SECRET is shorter than 32 chars — use a strong random secret");
    }
    if (cfg.mode == AuthMode::Bearer && cfg.bearer_token.empty()) {
        throw std::runtime_error("auth.mode=bearer but AUTH_BEARER_TOKEN is empty");
    }

    // Cookie session config — Config::get already resolves ${VAR}
    // placeholders during load_from_file → substitute_env_placeholders, so
    // the canonical layered lookup (env > json default > built-in default)
    // works for cookie fields too.
    cfg.cookies.enabled = c.get<bool>("auth.cookies.enabled", "AUTH_COOKIES_ENABLED", false);
    cfg.cookies.access_name = c.get<std::string>("auth.cookies.access_name", "AUTH_COOKIE_ACCESS", "__Host-access");
    cfg.cookies.refresh_name = c.get<std::string>("auth.cookies.refresh_name", "AUTH_COOKIE_REFRESH", "__Host-refresh");
    cfg.cookies.access_ttl_sec = c.get<int>("auth.cookies.access_ttl_sec", "AUTH_COOKIE_ACCESS_TTL_SEC", 15 * 60);
    cfg.cookies.refresh_ttl_sec =
        c.get<int>("auth.cookies.refresh_ttl_sec", "AUTH_COOKIE_REFRESH_TTL_SEC", 7 * 24 * 60 * 60);
    cfg.cookies.secure = c.get<bool>("auth.cookies.secure", "AUTH_COOKIE_SECURE", true);
    cfg.cookies.samesite = c.get<std::string>("auth.cookies.samesite", "AUTH_COOKIE_SAMESITE", "Lax");
    cfg.cookies.refresh_revocation_prefix =
        c.get<std::string>("auth.cookies.refresh_revocation_prefix", "AUTH_COOKIE_REVOCATION_PREFIX", "auth:refresh:");
    // CSRF lives under security.csrf.* but rides on the cookie config so
    // set_session_cookies can emit the token cookie. Off by default.
    cfg.cookies.csrf_enabled = c.get<bool>("security.csrf.enabled", "SECURITY_CSRF_ENABLED", false);
    cfg.cookies.csrf_cookie_name =
        c.get<std::string>("security.csrf.cookie_name", "SECURITY_CSRF_COOKIE", "csrf-token");
    spdlog::info("Auth cookies: enabled={} access_name={} samesite={} secure={}",
                 cfg.cookies.enabled,
                 cfg.cookies.access_name,
                 cfg.cookies.samesite,
                 cfg.cookies.secure);
    return cfg;
}

void initialize() {
    if (global_auth != nullptr) {
        // Same convention as RateLimit/Idempotency: repeated initialize is a
        // warned no-op (Core may be re-run in tests); use shutdown() first
        // to reconfigure.
        spdlog::warn("Auth::initialize called twice — keeping existing config");
        return;
    }
    auto cfg = load_config_from_global();
    global_auth = std::make_unique<Authenticator>(std::move(cfg));
    const char* mode_name = "none";
    switch (global_auth->config().mode) {
        case AuthMode::Bearer:
            mode_name = "bearer";
            break;
        case AuthMode::Jwt:
            mode_name = "jwt";
            break;
        case AuthMode::None:
            mode_name = "none";
            break;
    }
    spdlog::info(
        "Auth module initialized: mode={} public_paths={}", mode_name, global_auth->config().public_paths.size());
}

bool is_initialized() {
    return global_auth != nullptr;
}

Authenticator& get() {
    if (global_auth == nullptr) {
        throw std::runtime_error("Auth not initialized");
    }
    return *global_auth;
}

void shutdown() {
    global_auth.reset();
}

void install_for_testing(AuthConfig cfg) {
    global_auth = std::make_unique<Authenticator>(std::move(cfg));
}

std::optional<AuthPrincipal> principal_of(const drogon::HttpRequestPtr& req) {
    // Drogon's Attributes::get<T>() behaviour on a missing key differs by
    // version (throw std::out_of_range vs. return a default-constructed T) —
    // check presence explicitly so an absent principal can never masquerade
    // as an empty-subject one (which downstream code would feed to SQL).
    if (!req->attributes()->find(kPrincipalAttr))
        return std::nullopt;
    try {
        return req->attributes()->get<AuthPrincipal>(kPrincipalAttr);
    } catch (...) {
        return std::nullopt;
    }
}

bool has_role(const drogon::HttpRequestPtr& req, const std::string& role) {
    auto p = principal_of(req);
    if (!p)
        return false;
    return std::find(p->roles.begin(), p->roles.end(), role) != p->roles.end();
}

bool auth_enforced() {
    return is_initialized() && get().config().mode != AuthMode::None;
}

drogon::HttpResponsePtr require_role(const drogon::HttpRequestPtr& req, const std::string& role) {
    if (!auth_enforced())
        return {};
    if (has_role(req, role))
        return {};
    return ErrorResponse::make({drogon::k403Forbidden, "forbidden", "", nlohmann::json{{"required_role", role}}});
}

drogon::HttpResponsePtr require_confirmed(const drogon::HttpRequestPtr& req) {
    if (!auth_enforced())
        return {};
    auto p = principal_of(req);
    if (!p)
        return ErrorResponse::unauthorized();
    if (p->raw_claims.value("confirmed", false))
        return {};
    return ErrorResponse::make({drogon::k403Forbidden,
                                "email_unconfirmed",
                                "Confirm your email address to access this resource",
                                nlohmann::json{}});
}

std::uint32_t current_permissions(const drogon::HttpRequestPtr& req) {
    auto p = principal_of(req);
    if (!p)
        return 0;
    if (!p->raw_claims.contains("permissions"))
        return 0;
    const auto& v = p->raw_claims["permissions"];
    if (!v.is_number_integer())
        return 0;
    // get<int64_t>, not long: long is 32-bit on Windows, where a uint32 bitmask
    // with the high bit set would overflow on the way through.
    return static_cast<std::uint32_t>(v.get<std::int64_t>());
}

bool current_user_can(const drogon::HttpRequestPtr& req, std::uint32_t perm) {
    const std::uint32_t have = current_permissions(req);
    // The admin sentinel bit satisfies EVERY permission check — admin can do
    // anything. This was implicit when admin was 0xff (all low feature bits);
    // with the dedicated sentinel bit it must be explicit, or admins would be
    // rejected by feature-permission gates (e.g. Permission::kAuditRead).
    if ((have & kAdminPermissionBits) == kAdminPermissionBits)
        return true;
    return (have & perm) == perm;
}

drogon::HttpResponsePtr require_permission(const drogon::HttpRequestPtr& req, std::uint32_t perm) {
    if (!auth_enforced())
        return {};
    if (current_user_can(req, perm))
        return {};
    return ErrorResponse::make({drogon::k403Forbidden, "forbidden", "", nlohmann::json{{"required_permission", perm}}});
}

drogon::HttpResponsePtr require_admin(const drogon::HttpRequestPtr& req) {
    return require_permission(req, kAdminPermissionBits);
}

}  // namespace Security::Auth
