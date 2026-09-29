/**
 * @file Api.cpp
 * @brief Body for Api::register_controllers() — compiled once into app_core.
 *        The registration ORDER contract is documented on the declaration in
 *        Api.hpp; this TU is also one more includer of the controller
 *        headers, which is harmless (their Drogon self-registration statics
 *        are inline — one instance per program, however many TUs see them).
 */

#include "api/Api.hpp"

#include <spdlog/spdlog.h>

namespace Api {

void register_controllers() {
    spdlog::info("Registering API controllers");
    middleware::ensure_http_metric_families();
    middleware::register_request_id();
    middleware::register_content_type_check();
    middleware::register_auth();
    middleware::register_csrf();
    middleware::register_rate_limit();
    middleware::register_idempotency();
    middleware::register_cors();
    middleware::register_security_headers();
    middleware::register_tracing_pre();
    middleware::register_access_log_post();
    register_docs_endpoints();
}

}  // namespace Api
