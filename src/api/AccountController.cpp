/**
 * @file AccountController.cpp
 * @brief Bodies for src/api/AccountController.hpp — compiled once into
 *        app_core. The per-route flow contracts (idempotency, enumeration
 *        stance, one-shot token rules) are documented on the declarations
 *        in the header.
 */

#include "api/AccountController.hpp"

#include <exception>

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/HandlerSupport.hpp"
#include "api/Validation.hpp"
#include "database/Database.hpp"
#include "email/AccountEmails.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "security/Password.hpp"
#include "security/SessionStore.hpp"
#include "security/Tokens.hpp"
#include "utils/Crypto.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

void AccountController::resendConfirm(const HttpRequestPtr& req,
                                      std::function<void(const HttpResponsePtr&)>&& callback) {
    API_REQUIRE_PRINCIPAL(req, callback, principal);
    try {
        Repositories::UserRepository repo;
        auto user = repo.find(principal->subject);
        if (!user) {
            callback(ErrorResponse::not_found("user"));
            return;
        }
        if (user->confirmed) {
            callback(Response::ok({{"message", "already confirmed"}}));
            return;
        }
        Email::AccountEmails::send_confirm(*user);
        callback(Response::ok({{"message", "confirmation email sent"}}));
    } catch (const std::exception& e) {
        spdlog::error("resendConfirm failed: {}", e.what());
        callback(ErrorResponse::internal_error());
    }
}

void AccountController::confirm(const HttpRequestPtr& /*req*/,
                                std::function<void(const HttpResponsePtr&)>&& callback,
                                const std::string& token) {
    auto vr = Security::Tokens::verify(secret(), token, Security::Tokens::Purpose::Confirm);
    if (!vr.ok) {
        callback(ErrorResponse::bad_request("invalid_token", "Confirmation link is invalid or has expired"));
        return;
    }
    // One-shot, consistent with applyReset/applyChangeEmail: a captured
    // confirm link shouldn't stay replayable for the token's full (7-day)
    // lifetime. TTL matches the confirm token lifetime so the replay guard
    // outlives the token it protects.
    if (!consume_once(token, /*ttl_sec=*/7 * 24 * 3600)) {
        callback(ErrorResponse::bad_request("invalid_token", "Confirmation link has already been used"));
        return;
    }
    try {
        Repositories::UserRepository repo;
        if (!repo.mark_confirmed(vr.sub)) {
            callback(ErrorResponse::not_found("user"));
            return;
        }
        callback(Response::ok({{"message", "Account confirmed"}}));
    } catch (const std::exception& e) {
        spdlog::error("confirm failed: {}", e.what());
        callback(ErrorResponse::internal_error());
    }
}

void AccountController::requestReset(const HttpRequestPtr& req,
                                     std::function<void(const HttpResponsePtr&)>&& callback) {
    json body;
    if (!Validation::parse_body(req, body, callback))
        return;
    Validation::Errors errs;
    Validation::require(errs, body, "email");
    Validation::email(errs, body, "email");
    if (errs.any()) {
        callback(Validation::response_400(errs));
        return;
    }
    const std::string email = body["email"].get<std::string>();
    try {
        Repositories::UserRepository repo;
        auto user = repo.find_by_email(email);
        if (user) {
            Email::AccountEmails::send_reset(*user);
        } else {
            // Debug level, no address: the probed email is PII and an
            // info-level log would be a log-side enumeration channel
            // undercutting the deliberate generic 200 below.
            spdlog::debug("[reset-request] no matching user — silent ack");
        }
    } catch (const std::exception& e) {
        // Same generic 200 on backend trouble — a 500 here would leak
        // that the lookup ran (and retrying costs the user nothing).
        spdlog::error("requestReset failed: {}", e.what());
    }
    // Generic 200 either way — no enumeration.
    callback(Response::ok({{"message", "If that email is registered, a reset link is on its way."}}));
}

void AccountController::applyReset(const HttpRequestPtr& req,
                                   std::function<void(const HttpResponsePtr&)>&& callback,
                                   const std::string& token) {
    auto body = parse_new_password_body(req, callback);
    if (!body)
        return;
    auto vr = Security::Tokens::verify(secret(), token, Security::Tokens::Purpose::ResetPassword);
    if (!vr.ok) {
        callback(ErrorResponse::bad_request("invalid_token", "Reset link is invalid or has expired"));
        return;
    }
    // One-shot: a captured reset link must not be replayable to set the
    // password a second time after the legitimate user used it.
    if (!consume_once(token, /*ttl_sec=*/3600)) {
        callback(ErrorResponse::bad_request("invalid_token", "Reset link has already been used"));
        return;
    }
    with_repo_errors(callback, "applyReset", [&] {
        Repositories::UserRepository repo;
        const std::string new_hash = Security::Password::hash((*body)["new_password"].get<std::string>());
        repo.update_password_hash(vr.sub, new_hash);
        // Evict every existing session — a reset must lock out anyone
        // holding an old refresh token (incl. an attacker who triggered
        // the reset path). Best-effort; the new login mints a fresh one.
        Security::Sessions::revoke_all(Security::Auth::get().config().cookies, vr.sub);
        callback(Response::ok({{"message", "Password updated"}}));
    });
}

void AccountController::requestChangeEmail(const HttpRequestPtr& req,
                                           std::function<void(const HttpResponsePtr&)>&& callback) {
    API_REQUIRE_PRINCIPAL(req, callback, principal);
    json body;
    if (!Validation::parse_body(req, body, callback))
        return;
    Validation::Errors errs;
    Validation::require(errs, body, "new_email");
    Validation::email(errs, body, "new_email");
    // require_string: a non-string password reaches get<std::string>()
    // below and throws type_error.302 → bare 500 instead of a 400.
    Validation::require_string(errs, body, "password");
    if (errs.any()) {
        callback(Validation::response_400(errs));
        return;
    }
    try {
        auto user =
            verify_password_or_401(principal->subject, body["password"].get<std::string>(), "Wrong password", callback);
        if (!user)
            return;
        const std::string new_email = body["new_email"].get<std::string>();
        Email::AccountEmails::send_change_email(*user, new_email);
        callback(Response::ok({{"message", "Confirmation email sent to the new address."}}));
    } catch (const std::exception& e) {
        spdlog::error("requestChangeEmail failed: {}", e.what());
        callback(ErrorResponse::internal_error());
    }
}

void AccountController::applyChangeEmail(const HttpRequestPtr& /*req*/,
                                         std::function<void(const HttpResponsePtr&)>&& callback,
                                         const std::string& token) {
    auto vr = Security::Tokens::verify(secret(), token, Security::Tokens::Purpose::ChangeEmail);
    if (!vr.ok) {
        callback(ErrorResponse::bad_request("invalid_token", "Change-email link is invalid or has expired"));
        return;
    }
    const auto new_email_it = vr.extra.find("new_email");
    if (new_email_it == vr.extra.end() || !new_email_it->is_string()) {
        callback(ErrorResponse::bad_request("invalid_token", "Token is missing the new email"));
        return;
    }
    const std::string new_email = new_email_it->get<std::string>();
    // Check availability BEFORE consuming the one-shot token, so a
    // duplicate-email 409 doesn't permanently burn an otherwise-valid link.
    // change_email's UNIQUE constraint still guards the check→write race.
    {
        Repositories::UserRepository repo;
        auto taken = repo.find_by_email(new_email);
        if (taken && taken->id != vr.sub) {
            callback(ErrorResponse::conflict("email_taken", "That email address is already in use"));
            return;
        }
    }
    if (!consume_once(token, /*ttl_sec=*/3600)) {
        callback(ErrorResponse::bad_request("invalid_token", "Change-email link has already been used"));
        return;
    }
    with_repo_errors(callback, "applyChangeEmail", [&] {
        Repositories::UserRepository repo;
        repo.change_email(vr.sub, new_email);
        callback(Response::ok({{"message", "Email updated"}}));
    });
}

void AccountController::joinFromInvite(const HttpRequestPtr& req,
                                       std::function<void(const HttpResponsePtr&)>&& callback,
                                       const std::string& token) {
    auto body = parse_new_password_body(req, callback);
    if (!body)
        return;
    auto vr = Security::Tokens::verify(secret(), token, Security::Tokens::Purpose::Invite);
    if (!vr.ok) {
        callback(ErrorResponse::bad_request("invalid_token", "Invitation link is invalid or has expired"));
        return;
    }
    with_repo_errors(callback, "joinFromInvite", [&] {
        Repositories::UserRepository repo;
        const std::string new_hash = Security::Password::hash((*body)["new_password"].get<std::string>());
        // redeem_invite is a DB-level one-shot (only matches a still-pending
        // invite), so a replayed link can't reset an active account — no
        // need for the fail-open Redis guard here. Doing the write first
        // also means a transient failure doesn't burn the 7-day token.
        if (!repo.redeem_invite(vr.sub, new_hash)) {
            callback(ErrorResponse::bad_request("invalid_token",
                                                "This invitation has already been used or is no longer valid"));
            return;
        }
        // Parity with applyReset: drop any sessions for this subject.
        Security::Sessions::revoke_all(Security::Auth::get().config().cookies, vr.sub);
        callback(Response::ok({{"message", "Account ready — you can now sign in."}}));
    });
}

void AccountController::changePassword(const HttpRequestPtr& req,
                                       std::function<void(const HttpResponsePtr&)>&& callback) {
    API_REQUIRE_PRINCIPAL(req, callback, principal);
    json body;
    if (!Validation::parse_body(req, body, callback))
        return;
    Validation::Errors errs;
    // require_string on old_password: only new_password has a length
    // validator to reject a non-string, so an int here used to reach
    // get<std::string>() and throw type_error.302 → bare 500.
    Validation::require_string(errs, body, "old_password");
    Validation::require(errs, body, "new_password");
    Validation::string_length(errs, body, "new_password", Validation::kPasswordMinLen, Validation::kPasswordMaxLen);
    if (errs.any()) {
        callback(Validation::response_400(errs));
        return;
    }
    try {
        auto user = verify_password_or_401(
            principal->subject, body["old_password"].get<std::string>(), "Original password is incorrect", callback);
        if (!user)
            return;
        Repositories::UserRepository repo;
        const std::string new_hash = Security::Password::hash(body["new_password"].get<std::string>());
        repo.update_password_hash(user->id, new_hash);
        // Revoke other sessions on password change (the current client
        // will re-auth on its next refresh). Closes the "changed my
        // password but the thief stays logged in" gap.
        Security::Sessions::revoke_all(Security::Auth::get().config().cookies, user->id);
        callback(Response::ok({{"message", "Password updated"}}));
    } catch (const std::exception& e) {
        spdlog::error("changePassword failed: {}", e.what());
        callback(ErrorResponse::internal_error());
    }
}

const std::string& AccountController::secret() {
    return Security::Auth::get().config().jwt_secret;
}

std::optional<json> AccountController::parse_new_password_body(const HttpRequestPtr& req,
                                                               std::function<void(const HttpResponsePtr&)>& callback) {
    json body;
    if (!Validation::parse_body(req, body, callback))
        return std::nullopt;
    Validation::Errors errs;
    Validation::require(errs, body, "new_password");
    Validation::string_length(errs, body, "new_password", Validation::kPasswordMinLen, Validation::kPasswordMaxLen);
    if (errs.any()) {
        callback(Validation::response_400(errs));
        return std::nullopt;
    }
    return body;
}

std::optional<Domain::User> AccountController::verify_password_or_401(
    const std::string& subject,
    const std::string& password,
    const std::string& wrong_password_message,
    const std::function<void(const HttpResponsePtr&)>& callback) {
    Repositories::UserRepository repo;
    auto user = repo.find(subject);
    if (!user || !user->password_hash) {
        callback(ErrorResponse::unauthorized("invalid_credentials"));
        return std::nullopt;
    }
    if (!Security::Password::verify(password, *user->password_hash)) {
        callback(ErrorResponse::unauthorized("invalid_credentials", wrong_password_message));
        return std::nullopt;
    }
    return user;
}

bool AccountController::consume_once(const std::string& token, int ttl_sec) {
    try {
        const std::string hash = Utils::Crypto::sha256_hex(token);
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "INSERT INTO used_tokens (token_hash, expires_at) "
                "VALUES ($1, now() + make_interval(secs => $2)) "
                "ON CONFLICT (token_hash) DO NOTHING RETURNING token_hash",
                hash,
                ttl_sec);
            return !r.empty();  // a row came back ⇒ this is the first use
        });
    } catch (const std::exception& e) {
        spdlog::warn("consume_once: nonce write failed ({}) — refusing token (fail-closed)", e.what());
        return false;
    }
}

}  // namespace Api
