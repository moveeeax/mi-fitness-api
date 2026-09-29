/**
 * @file HealthController.cpp
 * @brief Bodies for src/api/HealthController.hpp — compiled once into
 *        app_core. Contract and probe semantics are documented on the
 *        declarations in the header. Core::* comes in transitively through
 *        the header — check-module-deps.sh's CORE_HPP_ALLOWED permits the
 *        direct core/Core.hpp include only there.
 */

#include "api/HealthController.hpp"

#include <ctime>

#include <nlohmann/json.hpp>

#include "api/Endpoints.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

std::string version_or_unknown() {
    return Core::is_initialized() ? Core::get().version() : std::string("unknown");
}

void HealthController::liveness(const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback) {
    callback(Response::ok({{"status", "alive"}, {"timestamp", std::time(nullptr)}}));
}

void HealthController::readiness(const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback) {
    // During graceful shutdown we must report NotReady so kube-proxy
    // removes us from the Service backends before Drogon stops accepting.
    if (Core::is_shutting_down()) {
        auto resp = Response::ok({{"status", "draining"}, {"timestamp", std::time(nullptr)}});
        resp->setStatusCode(k503ServiceUnavailable);
        callback(resp);
        return;
    }
    bool ready = Core::is_initialized() && Core::health_check();
    auto resp = Response::ok({{"status", ready ? "ready" : "not_ready"}, {"timestamp", std::time(nullptr)}});
    resp->setStatusCode(ready ? k200OK : k503ServiceUnavailable);
    callback(resp);
}

void HealthController::health(const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback) {
    // Pull every component registered via Core::register_health_check —
    // services that add their own modules no longer have to hard-code
    // lookups in this method.
    json components = json::object();
    bool critical_ok = true;         // a CRITICAL component is down → 503 unhealthy
    bool any_degraded_down = false;  // only OPTIONAL deps down → 200 degraded
    if (Core::is_initialized()) {
        for (const auto& c : Core::get().health_report()) {
            components[c.name] = {{"initialized", c.initialized}, {"healthy", c.healthy}, {"critical", c.critical}};
            if (!c.healthy) {
                if (c.critical)
                    critical_ok = false;
                else
                    any_degraded_down = true;
            }
        }
    } else {
        critical_ok = false;
    }
    // A degraded optional dependency (SMTP/storage/Kafka) reports "degraded"
    // but stays 200 — only a critical-component failure returns 503, matching
    // what /ready (Core::health_check) gates on.
    const char* status = !critical_ok ? "unhealthy" : (any_degraded_down ? "degraded" : "healthy");
    auto resp = Response::ok({{"status", status},
                              {"version", version_or_unknown()},
                              {"timestamp", std::time(nullptr)},
                              {"components", components}});
    resp->setStatusCode(critical_ok ? k200OK : k503ServiceUnavailable);
    callback(resp);
}

void RootController::getRoot(const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback) {
    json endpoints_json = json::array();
    for (const auto& ep : get_endpoints()) {
        endpoints_json.push_back({{"method", ep.method}, {"path", ep.path}, {"description", ep.description}});
    }
    callback(Response::ok(
        {{"message", "C++ API Template"}, {"version", version_or_unknown()}, {"endpoints", endpoints_json}}));
}

}  // namespace Api
