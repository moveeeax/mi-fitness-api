/**
 * @file GenericEmail.cpp
 * @brief Bodies for src/email/GenericEmail.hpp — compiled once into app_core.
 *        Holds the jobs/Jobs.hpp include that used to sit in the header, so
 *        the email→jobs edge stays out of the header plane (Mailer.cpp set
 *        the pattern). The enqueue-vs-inline and throw contracts are
 *        documented on the declarations in the header.
 */

#include "email/GenericEmail.hpp"

#include <stdexcept>
#include <string>

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "jobs/Jobs.hpp"
#include "utils/Strings.hpp"

namespace Email::SendEmail {

Email::Message message_from_payload(const json& payload) {
    Email::Message m;
    m.to = payload.value("to", "");
    m.subject = payload.value("subject", "");
    m.text_body = payload.value("text", "");
    m.html_body = payload.value("html", "");
    if (m.to.empty())
        throw std::runtime_error("email.send: 'to' is required");
    if (m.text_body.empty() && m.html_body.empty())
        throw std::runtime_error("email.send: needs a 'text' or 'html' body");
    return m;
}

json process_job(const json& payload) {
    Email::Message m = message_from_payload(payload);
    if (!Email::is_initialized())
        throw std::runtime_error("email.send: Mailer not initialized");
    if (!Email::get().send(m))
        throw std::runtime_error("email.send: SMTP refused mail to " + m.to);
    return json{{"sent", true}, {"to", m.to}};
}

void send(const std::string& to, const std::string& subject, const std::string& text, const std::string& html) {
    if (Email::detail::via_jobs()) {
        try {
            json payload = {{"to", to}, {"subject", subject}, {"text", text}};
            if (!html.empty())
                payload["html"] = html;
            auto job = Jobs::get().submit(kJobType, payload);
            spdlog::debug("SendEmail: to {} enqueued as job {}", Utils::Strings::mask_email(to), job.id);
            return;
        } catch (const std::exception& e) {
            spdlog::warn(
                "SendEmail: enqueue to {} failed ({}); sending inline", Utils::Strings::mask_email(to), e.what());
        }
    }
    try {
        if (!Email::is_initialized()) {
            spdlog::warn("SendEmail: mailer not initialized; dropping mail to {}", Utils::Strings::mask_email(to));
            return;
        }
        Email::Message m;
        m.to = to;
        m.subject = subject;
        m.text_body = text;
        m.html_body = html;
        if (!Email::get().send(m))
            spdlog::warn("SendEmail: SMTP refused mail to {}", Utils::Strings::mask_email(to));
    } catch (const std::exception& e) {
        spdlog::warn("SendEmail: failed to send to {}: {}", Utils::Strings::mask_email(to), e.what());
    }
}

}  // namespace Email::SendEmail
