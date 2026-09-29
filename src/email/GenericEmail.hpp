/**
 * @file GenericEmail.hpp
 * @brief Fire-and-forget transactional email for ANY app code — not tied to the
 *        account flows. `Email::SendEmail::send(to, subject, text, html)`
 *        enqueues an "email.send" job (retry/backoff/DLQ via Jobs, horizontal
 *        scale on the worker) when Jobs is up, else sends inline. Best-effort
 *        from the caller's side; the worker-side handler THROWS on SMTP refusal
 *        so Jobs drives at-least-once delivery.
 *
 * Declarations only — the bodies live in GenericEmail.cpp (compiled once
 * into app_core; ADR 0003 as amended 2026-08-22). That's also where the
 * jobs/Jobs.hpp include lives now — this header no longer contributes to
 * the email→jobs half of the include graph (same move as
 * Email::detail::via_jobs() in Mailer.cpp).
 *
 * Mirrors the AccountEmails enqueue/deliver split — see AccountEmails.hpp.
 */

#pragma once

#include <string>

#include <nlohmann/json_fwd.hpp>

#include "email/Mailer.hpp"

namespace Email::SendEmail {

using json = nlohmann::json;

/// Job type the worker subscribes to (WORKER_TYPES) to deliver ad-hoc mail.
inline constexpr const char* kJobType = "email.send";

/// Build a Message from the job payload. Validates required fields up front.
Email::Message message_from_payload(const json& payload);

/**
 * @brief Worker-side: deliver one ad-hoc email. THROWS on render/SMTP refusal so
 *        Jobs::fail() drives retry/backoff and eventually the DLQ.
 */
json process_job(const json& payload);

/**
 * @brief App-facing entry: send an arbitrary email. Enqueue for the worker when
 *        Jobs is up (retry/DLQ, scales out), else send inline. Best-effort —
 *        NEVER throws, so a transactional path can fire it without a try/catch.
 */
void send(const std::string& to, const std::string& subject, const std::string& text, const std::string& html = "");

}  // namespace Email::SendEmail
