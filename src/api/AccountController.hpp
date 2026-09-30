/**
 * @file AccountController.hpp
 * @brief Account self-service: change-password.
 *
 * Осталась одна ручка: почтовые флоу шаблона (confirm, reset,
 * change-email, invite) удалены зачисткой 2026-09-30 вместе с модулем
 * почты — SMTP в этом сервисе не настроен, роуты были мёртвые.
 *
 * Declarations only — the handler body lives in AccountController.cpp
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
    ADD_METHOD_TO(AccountController::changePassword, "/api/v1/account/change-password", Post);
    METHOD_LIST_END

    // ---------------------------------------------------------------------
    // POST /api/account/change-password  (auth required)
    //
    // Body: { old_password, new_password }. Verifies the old password
    // against the stored hash; updates the hash and revokes the other
    // sessions.
    // ---------------------------------------------------------------------
    void changePassword(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

private:
    /**
     * @brief Load the user behind @p subject and verify @p password against its
     *        stored hash. On a missing user/hash responds 401
     *        invalid_credentials (no message); on a failed verify responds 401
     *        invalid_credentials with @p wrong_password_message. Returns
     *        nullopt after responding.
     */
    static std::optional<Domain::User> verify_password_or_401(
        const std::string& subject,
        const std::string& password,
        const std::string& wrong_password_message,
        const std::function<void(const HttpResponsePtr&)>& callback);
};

}  // namespace Api
