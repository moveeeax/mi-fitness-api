/**
 * @file BuiltinHandlers.hpp
 * @brief Registers the job handlers the template ships with into the global
 *        Jobs::Dispatcher. Extracted from worker_main.cpp so the registration
 *        (e.g. "is account_email actually registered?") is unit-testable — the
 *        if-ladder→Dispatcher refactor could otherwise silently drop a handler
 *        and only a live worker run would notice.
 *
 * Declaration only — the body (and its email/webhooks dependencies) lives in
 * BuiltinHandlers.cpp, compiled once into app_core (ADR 0003 as amended
 * 2026-08-22). That keeps jobs headers free of email/webhooks includes: the
 * jobs→email header edge was one half of the email↔jobs include cycle.
 */

#pragma once

namespace Jobs {

/**
 * @brief Register the built-in handlers. Call once at worker startup. Add a new
 *        built-in in BuiltinHandlers.cpp, or have a handler header self-register
 *        via Jobs::JobHandlerRegistrar and just #include it from worker_main.cpp.
 */
void register_builtin_handlers();

}  // namespace Jobs
