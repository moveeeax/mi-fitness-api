/**
 * @file AuthController.cpp
 * @brief Bodies for src/api/AuthController.hpp — compiled once into
 *        app_core. Contract and flask-base parity notes are documented on
 *        the declarations in the header.
 */

#include "api/AuthController.hpp"

#include <cstdint>

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "api/HandlerSupport.hpp"
#include "api/Validation.hpp"
#include "cache/Cache.hpp"
#include "email/AccountEmails.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Audit.hpp"
#include "security/Jwt.hpp"
#include "security/Password.hpp"
#include "security/RateLimit.hpp"
#include "security/SessionStore.hpp"
#include "utils/Crypto.hpp"
#include "utils/ErrorResponse.hpp"
#include "utils/Time.hpp"

namespace Api {

void AuthController::registerUser(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    json body;
    if (!Validation::parse_body(req, body, callback))
        return;
    Validation::Errors errs;
    Validation::require(errs, body, "email");
    Validation::require(errs, body, "password");
    Validation::email(errs, body, "email");
    Validation::string_length(errs, body, "password", Validation::kPasswordMinLen, Validation::kPasswordMaxLen);
    if (errs.any()) {
        callback(Validation::response_400(errs));
        return;
    }

    const std::string email = body["email"].get<std::string>();
    const std::string password = body["password"].get<std::string>();
    const auto first_name = Validation::opt_string(body, "first_name");
    const auto last_name = Validation::opt_string(body, "last_name");

    Repositories::RoleRepository roles;
    auto default_role = roles.find_default();
    if (!default_role) {
        spdlog::error("No default role in DB — run migrations / setup-dev");
        callback(ErrorResponse::service_unavailable("misconfigured", "default role missing"));
        return;
    }

    // with_repo_errors centralizes the DuplicateEmail->409 / *->500 mapping
    // (was hand-rolled here, the exact drift the helper exists to prevent).
    with_repo_errors(callback, "register", [&] {
        const std::string hash = Security::Password::hash(password);
        Repositories::UserRepository users;
        auto created = users.create(email, hash, first_name, last_name, default_role->id, /*confirmed=*/false);

        // Attach the role we already loaded so to_json embeds it — no
        // need to re-query the row we just inserted.
        created.role = *default_role;
        // Fire the confirmation email. AccountEmails handles token
        // issuing + render + send; failures log but don't break
        // registration (the user still has an account, they can hit
        // /confirm-resend to retry).
        Email::AccountEmails::send_confirm(created);
        callback(Response::created(
            {{"user", json(created)}, {"message", "Account created. Check your email for the confirmation link."}}));
    });
}

void AuthController::login(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    json body;
    if (!Validation::parse_body(req, body, callback))
        return;
    Validation::Errors errs;
    // require_string, not require: a wrong-typed field ({"password": 123})
    // would otherwise reach get<std::string>() and throw type_error.302 —
    // a bare 500 on an unauthenticated endpoint instead of a 400.
    Validation::require_string(errs, body, "email");
    Validation::require_string(errs, body, "password");
    if (errs.any()) {
        callback(Validation::response_400(errs));
        return;
    }

    const std::string email = body["email"].get<std::string>();
    const std::string password = body["password"].get<std::string>();

    Repositories::UserRepository users;
    auto user = users.find_by_email(email);

    // Equalize timing across user-exists vs not. A missing user (or one with
    // no password hash) is verified against a fixed dummy hash so the ~90ms
    // argon2 cost is always paid — otherwise the short-circuit was a timing
    // oracle for user enumeration (argon2 is large enough to measure; DB
    // latency does not mask it). The dummy hash is computed once.
    static const std::string kDummyHash = Security::Password::hash("timing-equalizer-not-a-real-password");
    const std::string& hash_to_check = (user && user->password_hash) ? *user->password_hash : kDummyHash;
    const bool password_ok = Security::Password::verify(password, hash_to_check);

    if (!user || !user->password_hash || !password_ok) {
        // Audit the failed attempt so brute-force / credential-stuffing is
        // visible in the trail (it wasn't before — only successful admin
        // actions were recorded). No actor (unauthenticated); the attempted
        // email + source IP are the investigation handles. Use the shared
        // trusted-IP resolver (honors rate_limit.trust_proxy) — NOT a raw
        // X-Real-IP read, which is client-spoofable when not behind a proxy.
        const std::string ip = Security::RateLimit::client_ip(req);
        Security::Audit::record(
            /*actor_id=*/"", "auth.login_failed", "user", user ? user->id : "", {{"email", email}, {"ip", ip}});
        // Single message for missing-user + bad-password to defeat enumeration.
        callback(ErrorResponse::unauthorized("invalid_credentials", "Invalid email or password"));
        return;
    }

    // Issue access + refresh, write refresh JTI to Redis for revocation.
    auto session = mint_session(*user);
    if (!session) {
        callback(ErrorResponse::service_unavailable("session_unavailable", "Could not mint session"));
        return;
    }

    auto http = Response::ok({{"user", json(*user)}});
    Security::Auth::set_session_cookies(
        http, Security::Auth::get().config().cookies, session->access, session->refresh);
    callback(http);
}

void AuthController::logout(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    const auto& cfg = Security::Auth::get().config();
    const std::string refresh = Security::Auth::extract_refresh_token(req, cfg.cookies);
    if (!refresh.empty())
        revoke_refresh(cfg, refresh);

    auto http = Response::ok({{"message", "logged out"}});
    Security::Auth::set_session_cookies(http, cfg.cookies, "", "");
    callback(http);
}

void AuthController::refresh(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    const auto& cfg = Security::Auth::get().config();
    const std::string refresh = Security::Auth::extract_refresh_token(req, cfg.cookies);
    if (refresh.empty()) {
        callback(ErrorResponse::unauthorized("missing_refresh"));
        return;
    }

    std::string err;
    auto claims_opt = Security::Auth::verify_hs256_jwt(refresh, cfg.jwt_secret, err);
    if (!claims_opt) {
        callback(ErrorResponse::unauthorized(err));
        return;
    }
    const auto& claims = *claims_opt;
    if (claims.value("typ", "") != "refresh") {
        callback(ErrorResponse::unauthorized("not_a_refresh"));
        return;
    }
    const std::string sub = claims.value("sub", "");
    const std::string jti = claims.value("jti", "");
    if (sub.empty() || jti.empty()) {
        callback(ErrorResponse::unauthorized("malformed_claims"));
        return;
    }

    // Revocation check.
    if (!is_refresh_live(cfg, jti)) {
        callback(ErrorResponse::unauthorized("revoked"));
        return;
    }

    Repositories::UserRepository users;
    auto user = users.find(sub);
    if (!user) {
        // User deleted while session was active.
        revoke_refresh(cfg, refresh);  // best effort
        callback(ErrorResponse::unauthorized("user_gone"));
        return;
    }

    // Rotate.
    revoke_jti(cfg, jti);
    auto session = mint_session(*user);
    if (!session) {
        callback(ErrorResponse::service_unavailable("session_unavailable"));
        return;
    }
    auto http = Response::ok({{"user", *user}});
    Security::Auth::set_session_cookies(http, cfg.cookies, session->access, session->refresh);
    callback(http);
}

void AuthController::me(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    auto principal = Security::Auth::principal_of(req);
    if (!principal) {
        callback(ErrorResponse::unauthorized("missing_principal"));
        return;
    }
    Repositories::UserRepository users;
    auto user = users.find(principal->subject);
    if (!user) {
        callback(ErrorResponse::not_found("user"));
        return;
    }
    callback(Response::ok({{"user", *user}}));
}

std::string AuthController::make_jti() {
    return Utils::Crypto::random_hex(16);
}

std::optional<AuthController::Session> AuthController::mint_session(const Domain::User& user) {
    const auto& cfg = Security::Auth::get().config();
    if (cfg.jwt_secret.empty()) {
        spdlog::error("mint_session: JWT_SECRET unset");
        return std::nullopt;
    }
    const long now = Utils::Time::now_epoch_seconds();

    // Roles claim — string array, even for a single role, so the
    // existing AuthPrincipal extractor parses it consistently.
    json roles_array = json::array();
    if (user.role)
        roles_array.push_back(user.role->name);

    // Permissions bitmask in the JWT lets the request layer answer
    // require_permission(...) without re-loading the user from DB.
    // The bitmask matches Domain::Permission constants.
    const std::uint32_t perm_bits = user.role ? user.role->permissions : 0u;

    json access_claims = {
        {"sub", user.id},
        {"iat", now},
        {"exp", now + cfg.cookies.access_ttl_sec},
        {"typ", "access"},
        {"confirmed", user.confirmed},
        {"permissions", perm_bits},
        {cfg.jwt_roles_claim, roles_array},
    };
    if (!cfg.jwt_issuer.empty())
        access_claims["iss"] = cfg.jwt_issuer;
    if (!cfg.jwt_audience.empty())
        access_claims["aud"] = cfg.jwt_audience;

    const std::string jti = make_jti();
    json refresh_claims = {
        {"sub", user.id},
        {"iat", now},
        {"exp", now + cfg.cookies.refresh_ttl_sec},
        {"typ", "refresh"},
        {"jti", jti},
    };

    Session s;
    s.access = Security::Auth::issue_hs256_jwt(access_claims, cfg.jwt_secret);
    s.refresh = Security::Auth::issue_hs256_jwt(refresh_claims, cfg.jwt_secret);

    // Track the JTI (live-marker + per-user index for revoke-all). Redis
    // down → fail closed: a refresh we can't revoke is worse than a failed
    // login. record() returns false on the live-marker write failure.
    if (!Cache::is_initialized()) {
        spdlog::warn("Cache not initialized — refresh revocation will not work");
        return s;
    }
    if (!Security::Sessions::record(cfg.cookies, user.id, jti, cfg.cookies.refresh_ttl_sec)) {
        spdlog::error("mint_session: failed to record refresh JTI — refusing to mint session");
        return std::nullopt;
    }
    return s;
}

bool AuthController::is_refresh_live(const Security::Auth::AuthConfig& cfg, const std::string& jti) {
    return Security::Sessions::is_live(cfg.cookies, jti);
}

void AuthController::revoke_jti(const Security::Auth::AuthConfig& cfg, const std::string& jti) {
    Security::Sessions::revoke_jti(cfg.cookies, jti);
}

void AuthController::revoke_refresh(const Security::Auth::AuthConfig& cfg, const std::string& refresh_token) {
    std::string err;
    auto claims_opt = Security::Auth::verify_hs256_jwt(refresh_token, cfg.jwt_secret, err);
    if (!claims_opt)
        return;  // already invalid; nothing to revoke
    const std::string jti = claims_opt->value("jti", "");
    if (!jti.empty())
        revoke_jti(cfg, jti);
}

}  // namespace Api
