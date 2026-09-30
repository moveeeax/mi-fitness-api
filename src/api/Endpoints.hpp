/**
 * @file Endpoints.hpp
 * @brief Endpoint registry — the single source of truth for routes.
 * @details `docs/openapi.yaml` is checked against this list in CI via
 *          scripts/check-openapi-drift.sh; `--print-routes` prints it.
 *          Add a line here for every new ADD_METHOD_TO.
 */

#pragma once

#include <string>
#include <vector>

namespace Api {

/**
 * @brief Endpoint metadata: method, path, description
 */
struct EndpointInfo {
    std::string method;
    std::string path;
    std::string description;
};

/**
 * @brief Single source of truth for all registered API endpoints
 */
inline const std::vector<EndpointInfo>& get_endpoints() {
    static const std::vector<EndpointInfo> endpoints = {
        {"GET", "/", "Endpoint discovery"},
        {"GET", "/healthz", "Liveness probe"},
        {"GET", "/ready", "Readiness probe"},
        {"GET", "/health", "Detailed health check"},
        {"POST", "/api/v1/auth/login", "Log in (issues access + refresh cookies)"},
        {"POST", "/api/v1/auth/logout", "Log out (clears cookies + revokes refresh)"},
        {"POST", "/api/v1/auth/refresh", "Rotate access + refresh cookies"},
        {"GET", "/api/v1/auth/me", "Get the authenticated user"},
        {"POST", "/api/v1/account/change-password", "Change password while logged in"},
        {"GET", "/api/v1/account/api-keys", "List your API keys"},
        {"POST", "/api/v1/account/api-keys", "Create an API key (secret shown once)"},
        {"DELETE", "/api/v1/account/api-keys/{id}", "Revoke an API key"},
        {"GET", "/api/v1/admin/users", "Admin: list users"},
        {"POST", "/api/v1/admin/users", "Admin: create a user"},
        {"GET", "/api/v1/admin/users/{id}", "Admin: user detail"},
        {"PATCH", "/api/v1/admin/users/{id}", "Admin: update user (email, role, name)"},
        {"DELETE", "/api/v1/admin/users/{id}", "Admin: delete user"},
        {"GET", "/api/v1/admin/roles", "Admin: list roles"},
        {"POST", "/api/v1/admin/roles", "Admin: create role"},
        {"PATCH", "/api/v1/admin/roles/{id}", "Admin: update role (name, permissions, is_default)"},
        {"DELETE", "/api/v1/admin/roles/{id}", "Admin: delete role"},
        {"GET", "/api/v1/admin/audit", "Admin: list the audit trail (requires audit-read permission)"},
        {"GET", "/api/v1/jobs", "List jobs"},
        {"POST", "/api/v1/jobs", "Submit job"},
        {"GET", "/api/v1/jobs/dlq", "List dead-letter queue"},
        {"POST", "/api/v1/jobs/dlq/{id}/requeue", "Requeue a DLQ job"},
        {"GET", "/api/v1/jobs/{id}", "Get job status"},
        {"DELETE", "/api/v1/jobs/{id}", "Cancel job"},
        {"GET", "/api/v1/data/daily-activity", "Daily activity rows for a date range"},
        {"GET", "/api/v1/data/sleep", "Sleep sessions for a date range"},
        {"GET", "/api/v1/data/heart-rate", "Heart rate samples for a date range"},
        {"GET", "/api/v1/data/stress", "Stress samples for a date range"},
        {"GET", "/api/v1/data/spo2", "SpO2 samples for a date range"},
        {"GET", "/api/v1/data/body", "Body measurements for a date range"},
        {"GET", "/api/v1/data/workouts", "Workouts for a date range"},
        {"GET", "/api/v1/data/summary", "Per-day summary: steps, sleep, resting heart rate"},
        {"GET", "/api/v1/xiaomi/probe", "XiaomiController::listXiaomi"},
        {"POST", "/api/v1/sync", "Sync: enqueue a cloud sync run"},
        {"GET", "/api/v1/sync/{id}", "Sync: read a run's journal entry"},
    };
    return endpoints;
}

}  // namespace Api
