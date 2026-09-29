/**
 * @file BillingEmails.cpp
 * @brief Bodies for src/email/BillingEmails.hpp — compiled once into
 *        app_core. The never-throw contract and each function's dispatch
 *        rules are documented on the declarations in the header.
 */

#include "email/BillingEmails.hpp"

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "domain/User.hpp"
#include "email/GenericEmail.hpp"
#include "email/Templates.hpp"
#include "utils/Strings.hpp"
#include "utils/Time.hpp"

namespace Email::BillingEmails {

using json = nlohmann::json;

namespace detail {

/// cents -> "12.34" (or "-12.34" for a negative value) — integer-only, no
/// floating point anywhere in this conversion.
std::string format_cents(std::int64_t cents) {
    const bool negative = cents < 0;
    const std::int64_t magnitude = negative ? -cents : cents;
    const std::int64_t whole = magnitude / 100;
    const std::int64_t frac = magnitude % 100;
    std::string out = negative ? "-" : "";
    out += std::to_string(whole);
    out += ".";
    if (frac < 10)
        out += "0";
    out += std::to_string(frac);
    return out;
}

/// delta_credits -> "+42" / "-7" — plain signed integer formatting for the
/// admin adjustment notice. Unlike format_cents above, this is NOT a money
/// amount (no /100 cents conversion) — delta_credits is already a plain
/// credit count. std::to_string() already prepends '-' for a negative
/// value, so only the positive case needs an explicit '+'; no double
/// anywhere.
std::string format_signed_credits(std::int64_t delta_credits) {
    if (delta_credits >= 0)
        return "+" + std::to_string(delta_credits);
    return std::to_string(delta_credits);
}

/// Dispatch-time wall clock, ISO-8601 — the same choice receipt/refund/
/// failed's caller (BillingController's dispatch helpers) makes for those
/// emails. adjustment() has no stored historical timestamp to draw from at
/// all (it's a live admin action), so this module computes it directly
/// rather than taking it as a parameter.
std::string now_iso8601() {
    return Utils::Time::epoch_to_iso8601(Utils::Time::now_epoch_seconds());
}

/**
 * @brief Render + enqueue one billing email. NEVER throws — a render
 *        failure or an enqueue/send failure is logged and swallowed, the
 *        same contract as AccountEmails' controller-facing dispatch().
 *        app_name comes from Templates::default_context(), the same source
 *        every account email uses.
 */
void send_rendered(const std::string& tmpl, const std::string& subject, const Domain::User& user, json ctx) {
    try {
        json full = Email::Templates::default_context();
        full.update(ctx);
        full["user"] = json{{"full_name", user.full_name()}};
        auto rendered = Email::Templates::render_pair(tmpl, full);
        Email::SendEmail::send(user.email, subject, rendered.text, rendered.html);
    } catch (const std::exception& e) {
        spdlog::warn("BillingEmails: {} for {} failed: {}", tmpl, Utils::Strings::mask_email(user.email), e.what());
    }
}

}  // namespace detail

void receipt(const Domain::User& user,
             const std::string& package_title,
             std::int64_t amount_cents,
             const std::string& currency,
             std::int64_t credits,
             std::int64_t new_balance,
             const std::string& payment_id,
             const std::string& date) {
    try {
        json ctx;
        ctx["package_title"] = package_title;
        ctx["amount"] = detail::format_cents(amount_cents);
        ctx["currency"] = currency;
        ctx["credits"] = credits;
        ctx["new_balance"] = new_balance;
        ctx["payment_id"] = payment_id;
        ctx["date"] = date;
        detail::send_rendered("billing_receipt", "Your top-up receipt", user, ctx);
    } catch (const std::exception& e) {
        spdlog::warn("BillingEmails::receipt: failed for {}: {}", Utils::Strings::mask_email(user.email), e.what());
    }
}

void refund(const Domain::User& user,
            const std::string& kind_label,
            std::int64_t amount_cents,
            const std::string& currency,
            std::int64_t credits_deducted,
            std::int64_t new_balance,
            const std::string& payment_id,
            const std::string& date) {
    try {
        json ctx;
        ctx["kind_label"] = kind_label;
        ctx["amount"] = detail::format_cents(amount_cents);
        ctx["currency"] = currency;
        ctx["credits_deducted"] = credits_deducted;
        ctx["new_balance"] = new_balance;
        ctx["payment_id"] = payment_id;
        ctx["date"] = date;
        detail::send_rendered("billing_refund", kind_label + " processed", user, ctx);
    } catch (const std::exception& e) {
        spdlog::warn("BillingEmails::refund: failed for {}: {}", Utils::Strings::mask_email(user.email), e.what());
    }
}

void failed(const Domain::User& user,
            std::int64_t amount_cents,
            const std::string& currency,
            const std::string& reason,
            const std::string& date) {
    try {
        json ctx;
        ctx["amount"] = detail::format_cents(amount_cents);
        ctx["currency"] = currency;
        ctx["reason"] = reason;
        ctx["date"] = date;
        detail::send_rendered("billing_failed", "Your payment could not be completed", user, ctx);
    } catch (const std::exception& e) {
        spdlog::warn("BillingEmails::failed: failed for {}: {}", Utils::Strings::mask_email(user.email), e.what());
    }
}

void adjustment(const Domain::User& user,
                std::int64_t delta_credits,
                const std::string& reason,
                std::int64_t new_balance) {
    try {
        json ctx;
        ctx["delta_credits"] = detail::format_signed_credits(delta_credits);
        ctx["reason"] = reason;
        ctx["new_balance"] = new_balance;
        ctx["date"] = detail::now_iso8601();
        detail::send_rendered("billing_adjustment", "Your wallet balance was adjusted", user, ctx);
    } catch (const std::exception& e) {
        spdlog::warn("BillingEmails::adjustment: failed for {}: {}", Utils::Strings::mask_email(user.email), e.what());
    }
}

}  // namespace Email::BillingEmails
