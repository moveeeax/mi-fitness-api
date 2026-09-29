/**
 * @file AuditController.hpp
 * @brief Read-only admin view of the audit trail.
 *
 * Route:
 *   GET /api/admin/audit   list audit_log entries (paginated, newest first)
 *
 * Gated by the dedicated Permission::kAuditRead bit (not full-admin) so a
 * read-only "auditor" role is possible — full admins hold every 0xff bit and
 * pass automatically. Filters: ?action= &actor_id= &target_type= &from= &to=.
 *
 * Declarations only — the handler bodies live in AuditController.cpp
 * (compiled once into app_core; ADR 0003 as amended 2026-08-22). The route
 * macros (ADD_METHOD_TO) must stay in this header: Drogon's METHOD_LIST
 * registration is part of the class definition, and
 * scripts/check-routes-registered.sh greps the src/api headers for them.
 */

#pragma once

#include <functional>

#include <drogon/HttpController.h>

#include <nlohmann/json_fwd.hpp>

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class AuditController : public HttpController<AuditController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AuditController::listAudit, "/api/v1/admin/audit", Get);
    METHOD_LIST_END

    void listAudit(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
};

}  // namespace Api
