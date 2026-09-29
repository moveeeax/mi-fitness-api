/**
 * @file CurlInit.hpp
 * @brief One-shot global libcurl init shared by every curl user.
 * @details CURL needs `curl_global_init` once per process before any
 *          `curl_easy_*` calls. Sodium-style idempotent guard. Lives in
 *          utils/ so Mailer and Webhooks can share it without Webhooks
 *          having to include the whole Mailer (that edge made webhooks a
 *          transitive dependency of the email module and vice versa).
 */

#pragma once

#include <stdexcept>
#include <string>

#include <curl/curl.h>

namespace Utils {

inline void ensure_curl_init() {
    static const CURLcode rc = ::curl_global_init(CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK)
        throw std::runtime_error(std::string("curl_global_init failed: ") + curl_easy_strerror(rc));
}

}  // namespace Utils
