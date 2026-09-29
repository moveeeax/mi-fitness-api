/**
 * @file BillingController.hpp
 * @brief User-facing billing API: browse packages, top up (PayPal checkout),
 *        capture a return-flow order, and read your own wallet.
 * @details Every handler: module guard (require_billing_enabled) →
 *          API_REQUIRE_PRINCIPAL → Api::with_repo_errors. This controller
 *          NEVER writes wallet_entries/wallet_balances directly — every
 *          credit change routes through Billing::credit_capture()
 *          (src/billing/Wallet.hpp), the only code allowed to touch the
 *          ledger. PayPal is only ever reached through Billing::PayPalClient
 *          (src/billing/PayPalClient.hpp), whose install_for_testing() seam
 *          is what lets tests/api/test_billing_api.cpp exercise this
 *          controller with zero network calls.
 *
 * Declarations only — the handler bodies live in BillingController.cpp
 * (compiled once into app_core; ADR 0003 as amended 2026-08-22). The route
 * macros (ADD_METHOD_TO) must stay in this header: Drogon's METHOD_LIST
 * registration is part of the class definition, and
 * scripts/check-routes-registered.sh greps the src/api headers for them.
 *
 * Security invariants:
 *   - POST .../capture takes an order id from the request body. Before ever
 *     driving a capture, ownership is verified via PaymentRepository::
 *     find_owned (kOwnerColumn="user_id") — a caller can never capture (and
 *     collect credits for) an order that belongs to a different user.
 *   - POST .../topup computes credits SERVER-SIDE only, from either the
 *     package's own frozen `credits` column or `amount_cents *
 *     billing.credits_per_unit / 100` (integer math). Any "credits" field on
 *     the request body is never read anywhere in this controller — a client
 *     can send whatever it wants there and it has zero effect.
 *   - GET .../wallet takes no user-id parameter of any kind — it always
 *     reads the authenticated principal's own wallet.
 *   - Every amount is an integer (cents / credits); billing.min_amount_cents
 *     / billing.max_amount_cents bound a custom top-up amount.
 *
 * POST .../paypal/webhook — PayPal server-to-server notification, NOT a
 * user request:
 *   - Public (src/utils/Strings.hpp kDefaultPublicPathsCsv + config/config.json
 *     api.public_paths — BOTH, see that file's comment on why the config key
 *     overrides rather than merges) and CSRF-exempt by construction:
 *     Security::Csrf::passes() short-circuits on an empty access cookie, and
 *     PayPal never presents one (see Middleware.hpp / Csrf.hpp) — no
 *     path-based exemption needed.
 *   - The signature (PayPalClient::verify_webhook_signature) is checked
 *     against the RAW request body BEFORE that body is ever parsed as
 *     trusted JSON. No authenticated principal is required or possible.
 *   - Response codes are NOT the usual REST mapping — PayPal retries any
 *     non-2xx for days, so "handled" and "deliberately ignored" both answer
 *     200. See paypalWebhook()'s own doc comment for the exact 200/401/5xx
 *     rules. Everything past signature verification runs inside its own
 *     dedicated try/catch — deliberately NOT with_repo_errors (which every
 *     other handler in this controller uses): with_repo_errors maps
 *     NotFoundError/ConflictError/ValidationError to 404/409/400
 *     respectively, but a webhook has no REST-shaped caller to hand a 4xx
 *     to — every failure here (except the one narrow case documented on
 *     handleCaptureRefunded) must become a uniform 5xx so PayPal retries.
 *     This also closes a real crash class: a malformed-but-validly-signed
 *     body (e.g. "id" arriving as a JSON number) would otherwise throw
 *     straight out of this handler uncaught — Drogon does not catch that,
 *     so it takes the whole process down.
 *   - PAYMENT.CAPTURE.REVERSED (PayPal claws back a capture — chargeback/
 *     dispute/risk) is handled IDENTICALLY to PAYMENT.CAPTURE.REFUNDED
 *     (merchant-initiated): both are debit-shaped events per PayPal's own
 *     REST webhooks event-names reference, both DEBIT the wallet. See
 *     handleCaptureRefunded's doc comment.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>

#include <drogon/HttpController.h>

#include <nlohmann/json_fwd.hpp>

#include "billing/Wallet.hpp"
#include "domain/Billing.hpp"
#include "domain/User.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class BillingController : public HttpController<BillingController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(BillingController::listPackages, "/api/v1/billing/packages", Get);
    ADD_METHOD_TO(BillingController::getWallet, "/api/v1/billing/wallet", Get);
    ADD_METHOD_TO(BillingController::topup, "/api/v1/billing/topup", Post);
    ADD_METHOD_TO(BillingController::capture, "/api/v1/billing/capture", Post);
    ADD_METHOD_TO(BillingController::paypalWebhook, "/api/v1/billing/paypal/webhook", Post);
    METHOD_LIST_END

    // ── GET /api/v1/billing/packages ──────────────────────────────────────
    // Also returns the current per-unit rate and the custom-amount bounds —
    // a custom-amount input validates client-side against these, and the
    // client has no other way to learn them (they're never guessable from
    // the package list alone: a package's own price/credits ratio can
    // legitimately differ from the generic rate — see topup's doc comment).
    void listPackages(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

    // ── GET /api/v1/billing/wallet ────────────────────────────────────────
    // No user-id parameter of any kind is accepted — always the caller's own
    // wallet, resolved solely from the authenticated principal.
    void getWallet(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

    // ── POST /api/v1/billing/topup ────────────────────────────────────────
    void topup(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

    // ── POST /api/v1/billing/capture ──────────────────────────────────────
    void capture(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

    // ── POST /api/v1/billing/paypal/webhook ─────────────────────────────────
    // PayPal server-to-server notification. Public, unauthenticated, CSRF-
    // exempt by construction (see the class doc comment). Response codes:
    //   - 401: verify_webhook_signature() returned false (malformed body,
    //     missing paypal-* headers, or PayPal itself said the signature
    //     doesn't check out). Nothing is credited/refunded past this point.
    //   - 5xx: EITHER PayPal's OWN verify-webhook-signature API was
    //     unreachable/non-2xx (verify_webhook_signature() THROWS for this —
    //     see PayPalClient.hpp's class doc comment) OR processing a
    //     signature-VALID body failed for any reason at all — malformed
    //     shape (e.g. "id" arriving as a number, not a string), an
    //     unresolvable refund/reversal capture id, or Billing::refund_capture
    //     itself throwing (notably Repositories::PaymentNotFound, which can
    //     legitimately happen if this event races ahead of the
    //     CAPTURE.COMPLETED webhook that would have set
    //     provider_capture_id). ALL of that is caught by the ONE try/catch
    //     in the body — nothing after signature verification is allowed to
    //     escape this handler uncaught (Drogon does not catch it; an
    //     uncaught exception here crashes the process). 5xx tells PayPal to
    //     retry, which is the safe default: malformed-but-validly-signed is
    //     vanishingly rare, and silently dropping a DEBIT event (refund/
    //     reversal) into a 200 would be real, unrecoverable money loss.
    //   - 200: a credit successfully applied (or safely no-op'd) by
    //     handleCaptureCompleted, an already-processed refund/reversal
    //     replayed as a no-op by handleCaptureRefunded, or an event type
    //     this handler deliberately doesn't act on. PayPal retries any
    //     non-2xx for days, so every one of these must answer 200 or PayPal
    //     will hammer this endpoint forever for a condition retrying can
    //     never fix.
    //
    // Known limitations (inherited from the source fork, deliberately kept
    // rather than silently diverging from its audited behavior):
    //   - PAYMENT.CAPTURE.DENIED is not handled — a denied capture leaves the
    //     payment row 'created'/'approved' forever unless the return-flow
    //     capture endpoint happens to observe the failure; no failed-payment
    //     email fires for the webhook-only path.
    //   - A refund that is itself later VOIDED maps to no PayPal v2 webhook
    //     event at all (there is no PAYMENT.REFUND.REVERSED) — the debit
    //     stays applied; see handleCaptureRefunded's doc comment.
    //   - The admin payments list's ?status= filter (AdminBillingController::
    //     listPayments) is parameter-bound but not allow-list validated — an
    //     unknown status value returns an empty page rather than a 400.
    void paypalWebhook(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

private:
    /// Gate a billing-module handler: when the module is off
    /// (BILLING_ENABLED=false, the default) the whole surface answers 404
    /// instead of a 500 against missing tables/credentials. Kept
    /// controller-local because Guards.hpp has no generic per-module
    /// variant. Returns false
    /// after responding — callers `if (!require_billing_enabled(callback))
    /// return;`.
    static bool require_billing_enabled(const std::function<void(const HttpResponsePtr&)>& callback);

    // ── Billing email dispatch ──────────────────────────────────────────
    // Every function below is called AFTER a Billing::* wallet call has
    // already returned (credit_capture / refund_capture each fully commit
    // their own transaction internally) — never from inside
    // Database::execute_write, and never from inside a with_repo_errors
    // guarded lambda (capture() routes through the after_fn overload —
    // see HandlerSupport.hpp's incident note). Every one of them is itself
    // wrapped so a user-lookup failure, a missing payment row, or a
    // template error can NEVER affect the credit/refund/failure that
    // already committed; see BillingEmails.hpp's file comment for the same
    // contract one level down.

    /// Loads the user for an email dispatch. A missing user or a repo
    /// failure is logged and treated as "skip this email", never thrown.
    static std::optional<Domain::User> load_user_for_billing_email(const std::string& user_id);

    /// The receipt's "package" line: the catalogue title if this payment
    /// was a package purchase, else "Custom top-up" for a free-amount one
    /// (payments.package_id is optional for exactly this reason — see
    /// resolve_topup_plan).
    static std::string package_title_for(const Domain::Payment& payment);

    static std::string now_iso8601();

    /**
     * @brief Map `Wallet::credit_capture`'s INTERNAL `failure_reason` (e.g.
     *        "amount mismatch: captured=500 expected=1000") to a short,
     *        human sentence for the customer-facing failed-payment email.
     *        Only what this email RENDERS is translated — `payments.
     *        failure_reason` (DB, audit, admin payments list) keeps the
     *        internal diagnostic string verbatim; nothing here touches
     *        storage.
     *
     * credit_capture currently only ever writes an "amount mismatch" /
     * "currency mismatch" reason (or both, "; "-joined — see that
     * function's doc comment in Wallet.hpp), so both map to the same
     * customer sentence; anything else (should be unreachable today, since
     * that's the only internal reason producer, but defensive against a
     * future new one) gets a generic fallback. Both keep the "you were not
     * charged" reassurance — the internal strings never say that, and it's
     * the one thing a customer reading a payment-failed email most needs to
     * hear first.
     */
    static std::string friendly_failure_reason(const std::string& internal_reason);

    /// Send the receipt for a payment that was JUST credited
    /// (result.credited == true on the credit_capture call that produced
    /// @p result). Loads the payment (for amount/currency/package) and the
    /// user; best-effort throughout.
    static void dispatchReceiptEmail(const Billing::CreditResult& result);

    /**
     * @brief Send the failed-payment email once, at the amount/currency-
     *        mismatch transition credit_capture performs internally.
     *
     * credit_capture's CreditResult can't distinguish "this call just
     * caused the failed transition" from "already known (captured/failed)
     * before this call" — both return credited=false with the same shape.
     * @p was_failed_before is the caller's own pre-call read of the
     * payment's status, taken BEFORE the credit_capture call that produced
     * @p result: if the payment was already 'failed', this call can't be
     * the one that caused it, so nothing is sent (avoids a duplicate on a
     * retry/redelivery). If it wasn't, and the payment is 'failed' now,
     * this call caused it — send once.
     *
     * Known race (documented, not fixed — see Wallet.hpp's own KNOWN GAP
     * notes for the same class of tradeoff): the return-flow capture()
     * endpoint and a concurrent webhook delivery can both reach this point
     * for the same order; each one's own @p was_failed_before was read
     * before ITS credit_capture call, so both could read "not yet failed"
     * and both send a failed email once the race resolves. This is a rare
     * double-send, not a lost email, and not a money-safety issue — email
     * delivery here is explicitly best-effort.
     */
    static void dispatchFailedEmailIfJustTransitioned(const Billing::CreditResult& result, bool was_failed_before);

    static std::string title_case_refund_kind(const std::string& kind_label);

    /// The wallet_entries row a refund_capture call wrote, if it actually
    /// wrote one. refund_capture's CreditResult::credited is true both for
    /// an "applied" refund AND a durably-recorded-but-skipped one
    /// (insufficient balance / a sub-unit amount converting to 0 credits —
    /// see Wallet.hpp's refund_capture docs) — the ledger itself is the
    /// only place that distinguishes them, since a skipped attempt writes
    /// no wallet_entries row at all. Small bounded scan (newest 10) of the
    /// primary, run once right after the write.
    static std::optional<Domain::WalletEntry> find_refund_ledger_entry(const std::string& user_id,
                                                                       const std::string& provider_refund_id);

    /// Send the refund/reversal email ONLY when the refund actually debited
    /// the wallet on THIS call — see find_refund_ledger_entry's doc comment
    /// for why credited alone can't decide that.
    static void dispatchRefundEmailIfApplied(const Billing::CreditResult& result,
                                             const char* kind_label,
                                             const std::string& provider_refund_id,
                                             std::int64_t refunded_amount_cents,
                                             const std::string& currency);

    // Case-insensitive per Drogon's HttpRequest::getHeader — collects only
    // the five paypal-* headers verify_webhook_signature actually reads
    // (PayPalClient::detail::find_header_ci does its own case-insensitive
    // lookup within this map, so the case used as keys here doesn't matter).
    static std::map<std::string, std::string> collect_paypal_headers(const HttpRequestPtr& req);

    // PayPal's "up" link on a v2 refund resource points at
    // .../v2/payments/captures/{capture_id}[?query][#fragment] — the refund
    // resource itself carries no direct capture_id field. Anchored on the
    // literal "/captures/" marker (NOT "everything after the last '/'" — a
    // trailing '/', a '?query', or a '#fragment' after the id all silently
    // produce "" with the last-slash approach, permanently dropping a real
    // refund/reversal: capture_id="" → refund_capture() never called → 200
    // handled=false → PayPal never redelivers). Strips any query string,
    // fragment, and trailing slash from the extracted segment. Returns ""
    // only when no "up" link with a "/captures/" href is present at all (a
    // genuinely malformed/unexpected payload).
    static std::string extract_capture_id_from_links(const json& resource);

    // PAYMENT.CAPTURE.COMPLETED → credit. Handles BOTH "the user never
    // returned, this webhook is the only signal we ever get" AND "a capture
    // that was PENDING at return-flow time (BillingController::capture left
    // provider_capture_id NULL) now resolves to COMPLETED" — both funnel
    // into the exact same Billing::credit_capture call, whose own guarded
    // UPDATE (WHERE provider_capture_id IS NULL) makes a capture already
    // credited via the return flow a true no-op here (credited=false, same
    // balance, no second ledger row).
    static void handleCaptureCompleted(const json& event,
                                       const std::string& event_id,
                                       std::function<void(bool)> respond);

    // PAYMENT.CAPTURE.REFUNDED (merchant-initiated refund) and
    // PAYMENT.CAPTURE.REVERSED (PayPal claws back a capture — chargeback/
    // dispute/risk hold) share this ONE handler. Per PayPal's REST webhooks
    // event-names reference: REFUNDED = "A merchant refunds a payment
    // capture"; REVERSED = "PayPal reverses a payment capture". Both are
    // DEBIT-shaped events — money leaves the merchant either way — and both
    // carry the identical refund-shaped resource (id, amount,
    // links[rel=up]->capture), so both drive Billing::refund_capture
    // identically. A REFUNDED and a later REVERSED touching the same
    // underlying capture carry DISTINCT ids (this event's own resource.id
    // becomes provider_refund_id) — refund_capture's own idempotency (its
    // `billing_refunds` row) plus its cumulative-amount-vs-
    // payments.amount_cents cap already prevent the two from ever
    // double-debiting past the original payment amount, with no extra
    // bookkeeping needed here.
    //
    // (Separately: "a refund itself is later voided" maps to NO PayPal v2
    // webhook event at all — there is no PAYMENT.REFUND.REVERSED in PayPal's
    // event-names reference. That is a structural gap no webhook handler can
    // close by listening for more events; it stays an accepted, documented
    // limitation of Billing::refund_capture's `billing_refunds` cumulative
    // check, not something this handler failed to cover.)
    //
    // NOTHING here is caught locally — unlike handleCaptureCompleted, a
    // malformed resource, an unresolvable capture id, or refund_capture
    // itself throwing (notably Repositories::PaymentNotFound, which can
    // legitimately happen if this event is delivered before the
    // CAPTURE.COMPLETED webhook that would have set provider_capture_id) all
    // propagate to paypalWebhook's outer try/catch, which answers 5xx so
    // PayPal redelivers. A DEBIT event must never be silently 200-acked away
    // — that is real money gone with no way to recover it once PayPal stops
    // retrying.
    static void handleCaptureRefunded(const json& event,
                                      const std::string& event_id,
                                      const char* kind_label,  // "refund" or "reversal" — logging only
                                      std::function<void(bool)> respond);

    // Strips `created_by` (an admin's raw UUID on adjustment rows) before a
    // wallet_entries row is ever handed to an end user — Domain::to_json
    // includes it for internal/admin views, but this endpoint is user-facing.
    static json public_wallet_entry(const Domain::WalletEntry& e);

    // Config::parse_env_value<T> has no std::int64_t specialization (only
    // int/long/double/bool/string — see utils/Config.hpp) and silently falls
    // back to T{} = 0 for an unknown T, so every money-shaped config read
    // goes through `long` (this codebase's existing convention, e.g.
    // server.max_body_bytes in main.cpp) and is narrowed to std::int64_t
    // afterward, never read as std::int64_t directly.
    //
    // The rate/bounds are admin-editable at runtime via
    // PUT /api/v1/admin/billing/settings, persisted in `billing_settings`
    // (migration 008) — a redeploy-only config value can't satisfy that. The
    // migration seeds that row with the exact same numbers config.json
    // already ships (100 / 100 / 100000), so this is the SAME effective rate
    // for every environment until an admin actually changes it. Config is
    // kept only as a defensive fallback for the (should-be-impossible once
    // migrations have run) case of a missing row — never silently reading 0
    // and letting every top-up amount fail its range check.
    static void billing_limits(std::int64_t& rate, std::int64_t& min_cents, std::int64_t& max_cents);

    struct TopupPlan {
        std::int64_t amount_cents = 0;
        std::int64_t credits_expected = 0;
        std::int64_t rate_snapshot = 0;
        std::optional<std::string> package_id;
    };

    // Resolves amount_cents/credits_expected/rate_snapshot/package_id
    // entirely server-side. Deliberately never reads a "credits" key from
    // @p body at any point — the caller cannot influence the credited
    // amount by sending one. min/max_amount_cents is enforced on the
    // resulting amount_cents for BOTH branches — a misconfigured package
    // price outside the configured bounds is refused just like an
    // out-of-range custom amount, not silently sold.
    static bool resolve_topup_plan(const json& body,
                                   TopupPlan& out,
                                   const std::function<void(const HttpResponsePtr&)>& callback);
};

}  // namespace Api
