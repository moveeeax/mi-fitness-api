/**
 * @file HealthController.hpp
 * @brief Health check and root endpoint controllers
 * @details Kubernetes probes (/healthz, /ready, /health) and endpoint discovery (/)
 *
 * Declarations only — the handler bodies live in HealthController.cpp
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

// kept here because check-module-deps.sh's CORE_HPP_ALLOWED lists only this header; the body file receives it
// transitively.
#include "core/Core.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

/// Version string for status payloads: the Core-reported version once
/// initialized, "unknown" before that (shared by /health and /).
std::string version_or_unknown();

/**
 * @brief Health check controller
 * @details Provides liveness and readiness endpoints for Kubernetes
 */
class HealthController : public HttpController<HealthController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(HealthController::liveness, "/healthz", Get);
    ADD_METHOD_TO(HealthController::readiness, "/ready", Get);
    ADD_METHOD_TO(HealthController::health, "/health", Get);
    METHOD_LIST_END

    void liveness(const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback);

    void readiness(const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback);

    void health(const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback);
};

/**
 * @brief Root controller — lists available endpoints
 */
class RootController : public HttpController<RootController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(RootController::getRoot, "/", Get);
    METHOD_LIST_END

    void getRoot(const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback);
};

}  // namespace Api
