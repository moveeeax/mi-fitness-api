/**
 * @file ApiKeyController.cpp
 * @brief Bodies for src/api/ApiKeyController.hpp — compiled once into
 *        app_core. Contract and the show-the-secret-once rule are documented
 *        on the declarations in the header.
 */

#include "api/ApiKeyController.hpp"

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/HandlerSupport.hpp"
#include "api/Validation.hpp"
#include "repositories/ApiKeyRepository.hpp"
#include "security/ApiKeys.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

void ApiKeyController::list(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    API_REQUIRE_OWNER(req, callback, owner);
    Repositories::ApiKeyRepository repo;
    auto keys = repo.list_for_user(owner);
    const json data = to_json_array(keys);
    callback(Response::ok({{"data", data}, {"total", data.size()}}));
}

void ApiKeyController::create(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    API_REQUIRE_OWNER(req, callback, owner);
    json body;
    if (!Validation::parse_body(req, body, callback))
        return;
    Validation::Errors errs;
    // require_string: a non-string "name" would reach get<std::string>()
    // below and throw type_error.302 → bare 500 instead of a 400.
    Validation::require_string(errs, body, "name");
    if (errs.any()) {
        callback(Validation::response_400(errs));
        return;
    }

    const auto gen = Security::ApiKeys::generate();
    Repositories::ApiKeyRepository repo;
    auto key = repo.create(owner, body["name"].get<std::string>(), gen.key_hash, gen.prefix);

    // The plaintext key is surfaced ONCE here; it is never stored (only its
    // hash) and can never be shown again. The client must save it now.
    json out = key;
    out["key"] = gen.plaintext;
    callback(Response::created(out));
}

void ApiKeyController::remove(const HttpRequestPtr& req,
                              std::function<void(const HttpResponsePtr&)>&& callback,
                              const std::string& id) {
    API_REQUIRE_OWNER(req, callback, owner);
    Repositories::ApiKeyRepository repo;
    if (!repo.revoke(id, owner)) {
        // Not found, already revoked, or someone else's key — all the same
        // 404 so a caller can't probe which key ids exist.
        callback(ErrorResponse::not_found("api_key"));
        return;
    }
    callback(Response::ok({{"message", "API key revoked"}}));
}

}  // namespace Api
