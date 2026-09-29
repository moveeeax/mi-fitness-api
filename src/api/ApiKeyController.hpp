/**
 * @file ApiKeyController.hpp
 * @brief Manage the caller's own API keys: create / list / revoke. Owner-scoped
 *        (API_REQUIRE_OWNER) so a user only ever sees or revokes their own keys.
 *        The secret is returned exactly ONCE, from create().
 *
 * Declarations only — the handler bodies live in ApiKeyController.cpp
 * (compiled once into app_core; ADR 0003 as amended 2026-08-22). The route
 * macros (ADD_METHOD_TO) must stay in this header: Drogon's METHOD_LIST
 * registration is part of the class definition, and
 * scripts/check-routes-registered.sh greps the src/api headers for them.
 */

#pragma once

#include <functional>
#include <string>

#include <drogon/HttpController.h>

#include <nlohmann/json_fwd.hpp>

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class ApiKeyController : public HttpController<ApiKeyController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(ApiKeyController::list, "/api/v1/account/api-keys", Get);
    ADD_METHOD_TO(ApiKeyController::create, "/api/v1/account/api-keys", Post);
    ADD_METHOD_TO(ApiKeyController::remove, "/api/v1/account/api-keys/{1}", Delete);
    METHOD_LIST_END

    void list(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

    void create(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

    void remove(const HttpRequestPtr& req,
                std::function<void(const HttpResponsePtr&)>&& callback,
                const std::string& id);
};

}  // namespace Api
