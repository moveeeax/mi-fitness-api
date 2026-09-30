/**
 * @file Api.hpp
 * @brief API module aggregator: endpoint registry + controllers + middleware.
 * @details The layer is split so includers pay only for what they use:
 *            - Endpoints.hpp    — route registry (single source of truth)
 *            - RequestUtils.hpp — parse_int / clamp_int / pagination helpers
 *            - Guards.hpp       — handler-entry macros (admin / principal / jobs)
 *            - Middleware.hpp   — advice chain (declarations; the OTel glue
 *                                 lives in Middleware.cpp inside app_core)
 *          Controllers include the first three directly and must NOT include
 *          this header (that used to form an include cycle).
 *
 *          ONLY binary entry points include this header: src/main.cpp and
 *          tests/e2e (which boots the full server) — plus its own body file
 *          Api.cpp. Everything else — controllers, tests/api, tests/unit —
 *          includes the specific header it needs, so no controller is a
 *          transitive dependency of another and touching one controller
 *          recompiles one TU, not all.
 */

#pragma once

#include "api/Endpoints.hpp"
#include "api/RequestUtils.hpp"

// Controllers self-register with Drogon when their TU is compiled — pulling
// them in here is what puts the routes into main.cpp's binary.
#include "api/AccountController.hpp"
#include "api/AdminBillingController.hpp"
#include "api/AdminController.hpp"
#include "api/ApiKeyController.hpp"
#include "api/AuditController.hpp"
#include "api/AuthController.hpp"
#include "api/BillingController.hpp"
#include "api/HealthController.hpp"
#include "api/JobsController.hpp"
#include "api/SyncController.hpp"
#include "api/Middleware.hpp"
#include "api/XiaomiController.hpp"

namespace Api {

/**
 * @brief Register all API middleware (controllers register themselves).
 * @details The middleware order matters — each sync-advice runs in registration
 *          order and the FIRST one to return a response short-circuits the
 *          whole chain, INCLUDING every pre/post-handling advice (Drogon's
 *          passSyncAdvices() writes the response and returns false). That is
 *          why the sync advices are ordered as below and why each of them
 *          returns through middleware::short_circuit():
 *
 *            1. request id    — mints X-Request-Id / traceparent / the access
 *                               log's route + clock, so a response produced by
 *                               any advice below still carries them.
 *            2. content type  — reject a malformed mutation before paying for
 *                               auth / rate limit / idempotency lookups.
 *            3. auth          — unauthenticated requests don't consume the
 *                               rate-limit or idempotency budget.
 *            4. csrf          — cheap, cookie-only, runs on the authenticated
 *                               surface.
 *            5. rate limit
 *            6. idempotency   — needs the principal that auth stamped.
 *            7. cors          — answers the OPTIONS preflight.
 *
 *          Tracing is registered on the pre-handling path so the server span
 *          is opened only for requests that will actually reach a handler.
 *
 *          The body lives in Api.cpp (compiled once into app_core; ADR 0003
 *          as amended 2026-08-22) — it, not this header, pulls spdlog.
 */
void register_controllers();

}  // namespace Api
