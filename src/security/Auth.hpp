/**
 * @file Auth.hpp
 * @brief Authentication / authorization module.
 * @details Supports three modes:
 *          - "none"   — no auth (default, for local dev)
 *          - "bearer" — static Bearer token (legacy, for quick internal tests)
 *          - "jwt"    — HS256 JWT: signature + exp/nbf/iss/aud validation,
 *                       role-based authorization via claim (default "roles").
 *          The middleware is installed by Api::register_controllers(); RBAC
 *          helpers (require_role, has_role, principal_of) are available to
 *          any controller via Security::.
 *
 * Declarations only for the non-template bodies — they live in Auth.cpp
 * (compiled once into app_core; ADR 0003 as amended 2026-08-22). The enums,
 * structs and constants below stay here.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <nlohmann/json.hpp>

#include "security/SessionCookies.hpp"

namespace Security::Auth {

using json = nlohmann::json;

enum class AuthMode { None, Bearer, Jwt };

// Permission bitmask that denotes a full administrator: a DEDICATED sentinel
// bit, NOT 0xff "all bits" (with 0xff a role accumulating the eight low feature
// bits would accidentally become admin). Mirrors Domain::Permission::kAdminister
// — duplicated here (rather than including Domain) to keep the Auth layer below
// the Domain layer. MUST stay in sync; tests/unit/test_auth_permissions.cpp
// asserts equality so the two can't drift.
inline constexpr std::uint32_t kAdminPermissionBits = 0x40000000u;

struct AuthConfig {
    AuthMode mode = AuthMode::None;

    // Bearer mode.
    std::string bearer_token;

    // JWT HS256. Single shared secret — for kid-based key rotation, swap in
    // an external KMS or extend Authenticator with a JWKS resolver.
    std::string jwt_secret;
    std::string jwt_issuer;
    std::string jwt_audience;
    int jwt_leeway_sec = 30;
    std::string jwt_roles_claim = "roles";
    std::string jwt_scopes_claim = "scope";

    // Paths that never require auth. Exact-match only.
    std::unordered_set<std::string> public_paths;

    // Cookie-mode settings. Off by default; turn on for SPA flows.
    CookieConfig cookies;
};

struct AuthPrincipal {
    std::string subject;
    std::vector<std::string> roles;
    std::vector<std::string> scopes;
    // Active organization, from the access token's OPTIONAL "org" claim.
    // Empty when the claim is absent (a pre-multi-tenancy token, or a user in
    // 0/>1 orgs at mint time). Only consumed once the orgs starter kit is
    // installed (scripts/add-orgs.sh): Tenancy::org_context_of() treats an
    // empty value as fail-closed "no org access", never "unscoped access".
    std::string org;
    json raw_claims;
};

/**
 * @brief Authenticator: owns AuthConfig and verifies incoming tokens.
 */
class Authenticator {
public:
    explicit Authenticator(AuthConfig cfg) : config_(std::move(cfg)) {}

    const AuthConfig& config() const { return config_; }

    /**
     * @brief Verify a JWT Bearer token; populate principal on success.
     * @return empty optional + err code on failure.
     */
    std::optional<AuthPrincipal> verify_jwt(const std::string& token, std::string& err) const;

    bool path_is_public(const std::string& path) const;

private:
    AuthConfig config_;
};

// ---------------------------------------------------------------------------
// Global singleton
// ---------------------------------------------------------------------------

/**
 * @brief Build an AuthConfig from the process Config singleton.
 * @throws std::runtime_error if the mode requires a secret that is not set.
 */
AuthConfig load_config_from_global();

void initialize();

bool is_initialized();

Authenticator& get();

void shutdown();

// Test seam: install a global Authenticator from an explicit config, bypassing
// Config/env — so guards like require_confirmed are unit-testable in a chosen
// mode. Mirrors Cache::install_for_testing. Pair with shutdown() in TearDown.
void install_for_testing(AuthConfig cfg);

// ---------------------------------------------------------------------------
// RBAC helpers for controllers
// ---------------------------------------------------------------------------

inline constexpr const char* kPrincipalAttr = "_auth_principal";

std::optional<AuthPrincipal> principal_of(const drogon::HttpRequestPtr& req);

bool has_role(const drogon::HttpRequestPtr& req, const std::string& role);

/**
 * @brief True when auth checks must be enforced — the module is initialized
 *        and not running in AuthMode::None. Shared guard for the require_*
 *        helpers below, which all no-op when auth is disabled.
 */
bool auth_enforced();

/**
 * @brief Returns a 403 response if the request's principal lacks the role,
 *        or nullptr if it has the role (or auth is disabled).
 */
drogon::HttpResponsePtr require_role(const drogon::HttpRequestPtr& req, const std::string& role);

/**
 * @brief nullptr if the caller's email is confirmed (or auth is disabled);
 *        401 if anonymous; 403 if authenticated but unconfirmed. The "confirmed"
 *        boolean is read from the access JWT claim (minted at login), so no DB
 *        hit. flask-base parity: @confirmed_required. The flag is loaded
 *        everywhere but not enforced by default — gate your domain's
 *        confirmation-required routes with API_REQUIRE_CONFIRMED.
 */
drogon::HttpResponsePtr require_confirmed(const drogon::HttpRequestPtr& req);

// ---------------------------------------------------------------------------
// Permission bitmask helpers — flask-base parity: app/decorators.py
// permission_required / admin_required.
//
// The access JWT carries a "permissions" int claim whose bits match the
// constants in src/domain/Role.hpp (Permission::kGeneral, kAdminister, ...).
// Reading from the claim avoids a DB hit on every guarded request.
// ---------------------------------------------------------------------------

/**
 * @brief Bitmask of the current principal's permissions, or 0 if there
 *        isn't one. Read from the "permissions" claim of the access JWT.
 */
std::uint32_t current_permissions(const drogon::HttpRequestPtr& req);

bool current_user_can(const drogon::HttpRequestPtr& req, std::uint32_t perm);

/**
 * @brief Returns a 403 response if the request's principal lacks the
 *        permission, or nullptr if it has it (or auth is disabled).
 */
drogon::HttpResponsePtr require_permission(const drogon::HttpRequestPtr& req, std::uint32_t perm);

drogon::HttpResponsePtr require_admin(const drogon::HttpRequestPtr& req);

}  // namespace Security::Auth
