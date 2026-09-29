/**
 * @file AccountEmails.hpp
 * @brief Token-issuing email senders shared by Auth + Account + Admin
 *        controllers. The worker-side job handler lives in
 *        AccountEmailWorker.hpp.
 *
 * Declarations only — the bodies live in AccountEmails.cpp (compiled once
 * into app_core; ADR 0003 as amended 2026-08-22). That's also where the
 * jobs/Jobs.hpp include lives now — this header no longer contributes to
 * the email→jobs half of the include graph (same move as
 * Email::detail::via_jobs() in Mailer.cpp).
 *
 * Pulled out of AccountController because AuthController::registerUser
 * needs send_confirm_email() too, and pulling AccountController into
 * Auth (or vice versa) would cycle the include graph.
 *
 * Delivery is routed, not inlined (flask-base parity: app/email.py
 * pushes onto Flask-RQ):
 *
 *   send_*()  ──Jobs enabled──▶ Jobs::submit("account_email", payload)
 *      │                              │
 *      │ Jobs off / submit failed     ▼  worker (AccountEmailWorker.hpp)
 *      └────────▶ deliver_now() ◀── process_job()
 *
 * The controller-facing send_*() helpers stay best-effort: enqueue (or
 * inline-send) failures are logged, the user is acked regardless, so
 * SMTP outages don't expose a timing oracle. On the worker, however,
 * deliver_now() failures THROW — Jobs::fail() then drives retries with
 * backoff and eventually the DLQ, giving at-least-once delivery.
 *
 * Tokens are issued in deliver_now(), i.e. at actual send time on the
 * worker, so TTLs aren't eaten by queue latency.
 */

#pragma once

#include <string>

#include <nlohmann/json_fwd.hpp>

namespace Domain {
struct User;
}

namespace Email::AccountEmails {

using json = nlohmann::json;

/// Job type the worker must subscribe to (WORKER_TYPES) to deliver mail.
inline constexpr const char* kJobType = "account_email";

namespace detail {

/**
 * @brief Issue the token, build the template context and send — NOW, in
 *        this process. Shared by the worker handler and the inline
 *        fallback. Throws on unknown kind, render or send failure.
 *
 * @param new_email Only meaningful for kind == "change_email": the
 *                  address being adopted (and the recipient).
 */
void deliver_now(const std::string& kind, const Domain::User& user, const std::string& new_email);

}  // namespace detail

void send_confirm(const Domain::User& user);

void send_reset(const Domain::User& user);

void send_change_email(const Domain::User& user, const std::string& new_email);

void send_invite(const Domain::User& user);

}  // namespace Email::AccountEmails
