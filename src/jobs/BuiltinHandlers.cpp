/**
 * @file BuiltinHandlers.cpp
 * @brief Body for src/jobs/BuiltinHandlers.hpp — compiled once into app_core.
 *        The only jobs TU that sees the email/webhooks handlers it wires up.
 */

#include "jobs/BuiltinHandlers.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

#include <nlohmann/json.hpp>

#include "email/AccountEmailWorker.hpp"
#include "email/GenericEmail.hpp"
#include "jobs/Dispatcher.hpp"
#include "webhooks/Webhooks.hpp"

namespace Jobs {

void register_builtin_handlers() {
    auto& d = Dispatcher::get();
    // Account emails (confirm / reset / change-email / invite). Throws on
    // render/SMTP failure → retried, then DLQ'd.
    d.register_handler(Email::AccountEmails::kJobType,
                       [](const json& payload) { return Email::AccountEmails::process_job(payload); });
    // Generic ad-hoc email for any app code (not tied to account flows). Same
    // throw-on-failure → retry/DLQ contract.
    d.register_handler(Email::SendEmail::kJobType,
                       [](const json& payload) { return Email::SendEmail::process_job(payload); });
    // Outbound webhooks: signed POST to a subscriber URL, same retry/DLQ contract.
    d.register_handler(Webhooks::kJobType, [](const json& payload) { return Webhooks::process_job(payload); });
    // Demo handlers used by examples/tests.
    d.register_handler("echo", [](const json& payload) { return payload; });
    d.register_handler("slow", [](const json& payload) -> json {
        int seconds = 2;
        if (payload.contains("seconds") && payload["seconds"].is_number())
            seconds = std::max(1, std::min(payload["seconds"].get<int>(), 30));
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        return {{"slept_seconds", seconds}, {"message", "done"}};
    });
    d.register_handler("fail",
                       [](const json&) -> json { throw std::runtime_error("intentional failure for testing"); });
}

}  // namespace Jobs
