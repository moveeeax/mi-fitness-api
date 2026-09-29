/**
 * @file BillingEmails.hpp
 * @brief Best-effort transactional emails for the billing module: top-up
 *        receipts, refund/reversal notices, failed-payment notices, and
 *        admin wallet-adjustment notices.
 *
 * Declarations only — the bodies live in BillingEmails.cpp (compiled once
 * into app_core; ADR 0003 as amended 2026-08-22).
 *
 * Routed through Email::SendEmail::send() (src/email/GenericEmail.hpp) — the
 * generic ad-hoc "email.send" job type, NOT a new job kind. Every public
 * function here (receipt/refund/failed/adjustment) wraps its entire body in
 * try/catch and NEVER throws: a template error, a missing user, or a mail
 * outage must never surface into money code. Callers (BillingController /
 * AdminBillingController) are required to dispatch these AFTER a
 * Billing::* wallet call has already returned — never from inside
 * Database::execute_write, and never from inside a with_repo_errors guarded
 * lambda (use the after_fn overload — see HandlerSupport.hpp's incident
 * note). See BillingController's dispatch helpers for how each call
 * site decides WHETHER to send (the credited/applied/failed dedupe logic
 * lives there, not here — this module only renders and sends).
 *
 * Money formatting: cents -> "12.34" is plain integer division/modulo
 * (amount_cents / 100, amount_cents % 100, zero-padded) — no double
 * anywhere, matching this codebase's money-handling convention (see
 * Wallet.hpp).
 */

#pragma once

#include <cstdint>
#include <string>

namespace Domain {
struct User;
}

namespace Email::BillingEmails {

/**
 * @brief Top-up receipt. Callers must send this ONLY when
 *        Billing::CreditResult::credited == true on the credit_capture call
 *        that produced it — that flag is exactly what makes a return-flow
 *        capture and a webhook for the SAME payment produce one receipt,
 *        never two (see BillingController::capture / handleCaptureCompleted).
 */
void receipt(const Domain::User& user,
             const std::string& package_title,
             std::int64_t amount_cents,
             const std::string& currency,
             std::int64_t credits,
             std::int64_t new_balance,
             const std::string& payment_id,
             const std::string& date);

/**
 * @brief Refund/reversal notice. Callers must send this ONLY when the
 *        underlying refund_capture call actually debited the wallet (ledger
 *        outcome "applied") — never for a redelivery (credited == false)
 *        and never for a durably-recorded-but-skipped attempt (insufficient
 *        balance, or a sub-unit amount that converts to 0 credits). See
 *        BillingController::dispatchRefundEmailIfApplied for how "applied"
 *        is determined.
 *
 * @param kind_label Human label — "Refund" or "Reversal".
 */
void refund(const Domain::User& user,
            const std::string& kind_label,
            std::int64_t amount_cents,
            const std::string& currency,
            std::int64_t credits_deducted,
            std::int64_t new_balance,
            const std::string& payment_id,
            const std::string& date);

/**
 * @brief Failed-payment notice — sent once, at the amount/currency-mismatch
 *        transition inside Billing::credit_capture (the payment is marked
 *        'failed', wallet left untouched). The template MUST make clear the
 *        user was NOT charged — the mismatch is caught before any credit is
 *        ever applied.
 */
void failed(const Domain::User& user,
            std::int64_t amount_cents,
            const std::string& currency,
            const std::string& reason,
            const std::string& date);

/**
 * @brief Admin wallet-adjustment notice. Callers must send this ONLY when
 *        the admin explicitly opted in via the `notify` flag on
 *        POST .../admin/billing/users/{id}/adjust AND Billing::adjust plus
 *        its audit row have already both succeeded — Billing::adjust itself
 *        has no notion of notification, so AdminBillingController::
 *        adjustWallet decides whether to call this at all, from its
 *        with_repo_errors after_fn (never inside the guarded lambda),
 *        mirroring capture()'s dispatch pattern in BillingController.hpp.
 *
 * @param delta_credits Signed adjustment; rendered as "+250" / "-50" (see
 *        detail::format_signed_credits in BillingEmails.cpp) — plain integer
 *        formatting, no double anywhere, matching this module's
 *        money-handling convention.
 * @param reason The admin's mandatory `note` from the request, reused
 *        verbatim as the notice's reason — adjustWallet has no separate
 *        "reason" field.
 * @param new_balance The wallet balance AFTER this adjustment
 *        (Billing::CreditResult::balance from the adjust() call that
 *        produced it).
 */
void adjustment(const Domain::User& user,
                std::int64_t delta_credits,
                const std::string& reason,
                std::int64_t new_balance);

}  // namespace Email::BillingEmails
