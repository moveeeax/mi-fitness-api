/**
 * @file AdminController.hpp
 * @brief Admin user-management endpoints.
 *
 * flask-base parity: app/admin/views.py — same routes, JSON shape
 * instead of HTML+flash. Every handler is gated by require_admin().
 *
 * Routes (all under /api/admin):
 *   GET    /api/admin/users                      list users (paginated)
 *   POST   /api/admin/users                      create a fully-formed user
 *   POST   /api/admin/invite                     invite via email — user sets password later
 *   GET    /api/admin/users/{id}                 user detail
 *   PATCH  /api/admin/users/{id}                 partial update (email / role / first/last name)
 *   DELETE /api/admin/users/{id}                 delete user
 *   GET    /api/admin/roles                      list roles
 *
 * Declarations only — the handler bodies live in AdminController.cpp
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

#include "domain/Role.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class AdminController : public HttpController<AdminController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AdminController::listUsers, "/api/v1/admin/users", Get);
    ADD_METHOD_TO(AdminController::createUser, "/api/v1/admin/users", Post);
    ADD_METHOD_TO(AdminController::inviteUser, "/api/v1/admin/invite", Post);
    ADD_METHOD_TO(AdminController::getUser, "/api/v1/admin/users/{1}", Get);
    ADD_METHOD_TO(AdminController::updateUser, "/api/v1/admin/users/{1}", Patch);
    ADD_METHOD_TO(AdminController::deleteUser, "/api/v1/admin/users/{1}", Delete);
    ADD_METHOD_TO(AdminController::listRoles, "/api/v1/admin/roles", Get);
    ADD_METHOD_TO(AdminController::createRole, "/api/v1/admin/roles", Post);
    ADD_METHOD_TO(AdminController::updateRole, "/api/v1/admin/roles/{1}", Patch);
    ADD_METHOD_TO(AdminController::deleteRole, "/api/v1/admin/roles/{1}", Delete);
    METHOD_LIST_END

    void listUsers(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

    void createUser(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

    void inviteUser(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

    void getUser(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback,
                 const std::string& id);

    void updateUser(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback,
                    const std::string& id);

    void deleteUser(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback,
                    const std::string& id);

    void listRoles(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

    void createRole(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

    void updateRole(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback,
                    const std::string& id_str);

    void deleteRole(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback,
                    const std::string& id_str);

private:
    /// Acting admin's principal subject for the audit trail ("" when auth off).
    static std::string actor_of(const HttpRequestPtr& req);

    /**
     * @brief Resolve the optional "role_id" in @p body (defaults to the
     *        default role when absent) — the shared preamble of createUser and
     *        inviteUser. On an unknown role responds 400 invalid_role with
     *        @p invalid_message (the two callers word it differently; "" omits
     *        the message key) and returns nullopt.
     */
    static std::optional<Domain::Role> resolve_role(const json& body,
                                                    const std::string& invalid_message,
                                                    const std::function<void(const HttpResponsePtr&)>& callback);

    /// Reject a malformed user-id path param with the admin surface's
    /// published 400 shape — code "invalid_id" (NOT Guards' "invalid_uuid").
    /// Returns false after responding — callers
    /// `if (!require_user_id(id, callback)) return;`.
    static bool require_user_id(const std::string& id, const std::function<void(const HttpResponsePtr&)>& callback);

    /// Parse a role-id path param; a non-positive or non-numeric value gets
    /// the shared bare 400 invalid_id. Returns nullopt after responding.
    static std::optional<int> require_role_id(const std::string& id_str,
                                              const std::function<void(const HttpResponsePtr&)>& callback);
};

}  // namespace Api
