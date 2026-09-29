/**
 * @file AuditController.cpp
 * @brief Bodies for src/api/AuditController.hpp — compiled once into
 *        app_core. Contract and the kAuditRead permission gating are
 *        documented on the declarations in the header.
 */

#include "api/AuditController.hpp"

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/HandlerSupport.hpp"
#include "api/RequestUtils.hpp"
#include "domain/AuditEntry.hpp"
#include "domain/Role.hpp"
#include "repositories/AuditRepository.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

void AuditController::listAudit(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    API_REQUIRE_PERMISSION(req, callback, Domain::Permission::kAuditRead);

    const auto pp = parse_page_params(req, /*default_limit=*/50, /*max_limit=*/200);

    auto param = [&](const char* key) -> std::optional<std::string> {
        auto v = req->getParameter(key);
        return v.empty() ? std::nullopt : std::optional<std::string>{v};
    };
    Repositories::AuditRepository::Filters f;
    f.action = param("action");
    f.actor_id = param("actor_id");
    f.target_type = param("target_type");
    f.from = param("from");
    f.to = param("to");

    with_repo_errors(callback, "admin listAudit", [&] {
        Repositories::AuditRepository repo;
        auto page = repo.list_filtered(f, pp.limit, pp.offset);
        callback(Response::paginated(to_json_array(page.entries), page.total, pp.limit, pp.offset));
    });
}

}  // namespace Api
