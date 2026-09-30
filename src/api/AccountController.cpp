/**
 * @file AccountController.cpp
 * @brief Body for src/api/AccountController.hpp — compiled once into
 *        app_core.
 */

#include "api/AccountController.hpp"

#include <exception>

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/HandlerSupport.hpp"
#include "api/Validation.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "security/Password.hpp"
#include "security/SessionStore.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

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

}  // namespace Api
