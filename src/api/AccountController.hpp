/**
 * @file AccountController.hpp
 * @brief Account self-service: confirm-email, reset-password, change-email,
 *        change-password, resend-confirm.
 *
 * flask-base parity: app/account/views.py — same flows, JSON endpoints
 * instead of HTML+flash. Token routes accept the token as a path
 * segment so links in emails Just Work without query-string mangling.
 *
 * All these handlers are intentionally minimal — render template,
 * issue Tokens, persist via UserRepository. No business logic beyond
 * what flask-base already specified.
 *
 * Declarations only — the handler bodies live in AccountController.cpp
 * (compiled once into app_core; ADR 0003 as amended 2026-08-22). The route
 * macros (ADD_METHOD_TO) must stay in this header: Drogon's METHOD_LIST
 * registration is part of the class definition, and
 * scripts/check-routes-registered.sh greps the src/api headers for them.
 */

#pragma once

#include <functional>
#include <optional>
#include <string>

#include <drogon/HttpController.h>

#include <nlohmann/json_fwd.hpp>

#include "domain/User.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class AccountController : public HttpController<AccountController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AccountController::resendConfirm, "/api/v1/account/confirm-resend", Post);
    ADD_METHOD_TO(AccountController::confirm, "/api/v1/account/confirm/{1}", Post);
    ADD_METHOD_TO(AccountController::requestReset, "/api/v1/account/reset-password-request", Post);
    ADD_METHOD_TO(AccountController::applyReset, "/api/v1/account/reset-password/{1}", Post);
    ADD_METHOD_TO(AccountController::requestChangeEmail, "/api/v1/account/change-email-request", Post);
    ADD_METHOD_TO(AccountController::applyChangeEmail, "/api/v1/account/change-email/{1}", Post);
    ADD_METHOD_TO(AccountController::joinFromInvite, "/api/v1/account/join-from-invite/{1}", Post);
    ADD_METHOD_TO(AccountController::changePassword, "/api/v1/account/change-password", Post);
    METHOD_LIST_END

    // ---------------------------------------------------------------------
    // POST /api/account/confirm-resend  (auth required)
    //
    // Re-send the confirmation email for the *current* user. Idempotent:
    // even an already-confirmed user can request, but we return 200 with
    // a no-op message rather than firing a redundant token.
    // ---------------------------------------------------------------------
    void resendConfirm(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

    // ---------------------------------------------------------------------
    // POST /api/account/confirm/{token}
    //
    // Public — the link in the email points here. Verifies token against
    // Confirm purpose, marks user confirmed.
    // ---------------------------------------------------------------------
    void confirm(const HttpRequestPtr& /*req*/,
                 std::function<void(const HttpResponsePtr&)>&& callback,
                 const std::string& token);

    // ---------------------------------------------------------------------
    // POST /api/account/reset-password-request
    //
    // Public. Body: { email }. Always returns 200 — we never reveal
    // whether the email is registered (flask-base does the same with
    // its flash message wording).
    // ---------------------------------------------------------------------
    void requestReset(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

    // ---------------------------------------------------------------------
    // POST /api/account/reset-password/{token}
    //
    // Body: { new_password }. Verifies token, sets a fresh password hash.
    // ---------------------------------------------------------------------
    void applyReset(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback,
                    const std::string& token);

    // ---------------------------------------------------------------------
    // POST /api/account/change-email-request   (auth required)
    //
    // Body: { new_email, password }. Verifies current password, mints
    // a token bearing the new_email, sends confirmation email to the
    // *new* address (not the old one — flask-base behaviour).
    // ---------------------------------------------------------------------
    void requestChangeEmail(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

    // ---------------------------------------------------------------------
    // POST /api/account/change-email/{token}
    //
    // Verifies token, changes email atomically. Fails 409 if the new
    // address has been registered by someone else in the meantime.
    // ---------------------------------------------------------------------
    void applyChangeEmail(const HttpRequestPtr& /*req*/,
                          std::function<void(const HttpResponsePtr&)>&& callback,
                          const std::string& token);

    // ---------------------------------------------------------------------
    // POST /api/account/join-from-invite/{token}
    //
    // Public — the link in the admin invitation email points here. Verifies
    // the Invite token, sets the invitee's first password and confirms the
    // account in a single write. flask-base parity: /join-from-invite/<token>.
    // ---------------------------------------------------------------------
    void joinFromInvite(const HttpRequestPtr& req,
                        std::function<void(const HttpResponsePtr&)>&& callback,
                        const std::string& token);

    // ---------------------------------------------------------------------
    // POST /api/account/change-password   (auth required)
    //
    // Body: { old_password, new_password }. Verifies the old password
    // against the stored hash; updates the hash. Doesn't invalidate
    // existing sessions — that's a deliberate trade-off matching
    // flask-base. If you need session-rotation, mint a fresh refresh
    // pair after the change.
    // ---------------------------------------------------------------------
    void changePassword(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

private:
    static const std::string& secret();

    /**
     * @brief Parse the request body and validate its "new_password" field
     *        (required + kPasswordMinLen..kPasswordMaxLen) — the shared
     *        preamble of applyReset and joinFromInvite. Responds via
     *        @p callback itself and returns nullopt on any failure.
     */
    static std::optional<json> parse_new_password_body(const HttpRequestPtr& req,
                                                       std::function<void(const HttpResponsePtr&)>& callback);

    /**
     * @brief Load the user behind @p subject and verify @p password against its
     *        stored hash — the shared preamble of requestChangeEmail and
     *        changePassword. On a missing user/hash responds 401
     *        invalid_credentials (no message); on a failed verify responds 401
     *        invalid_credentials with @p wrong_password_message (the two
     *        callers word it differently). Returns nullopt after responding.
     */
    static std::optional<Domain::User> verify_password_or_401(
        const std::string& subject,
        const std::string& password,
        const std::string& wrong_password_message,
        const std::function<void(const HttpResponsePtr&)>& callback);

    /**
     * @brief Atomically consume a one-shot token: returns true the FIRST time
     *        this token is seen, false on every replay. DB-authoritative via the
     *        used_tokens table (migration 002) + INSERT ... ON CONFLICT DO
     *        NOTHING — so a captured token CANNOT be replayed during a Redis
     *        outage the way the old cache-only nonce (fail-open) allowed.
     *        Fail-CLOSED on a DB error (returns false): the guarded action needs
     *        the DB anyway, so refusing the token is the safe choice. Never
     *        throws — callers consume it outside with_repo_errors.
     */
    static bool consume_once(const std::string& token, int ttl_sec);
};

}  // namespace Api
