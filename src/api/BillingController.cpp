/**
 * @file BillingController.cpp
 * @brief Bodies for src/api/BillingController.hpp — compiled once into
 *        app_core. Contract, security invariants and the webhook's
 *        200/401/5xx rules are documented on the declarations in the header.
 */

#include "api/BillingController.hpp"

#include <stdexcept>
#include <string_view>

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/HandlerSupport.hpp"
#include "api/RequestUtils.hpp"
#include "api/Validation.hpp"
#include "billing/PayPalClient.hpp"
#include "core/Modules.hpp"
#include "email/BillingEmails.hpp"
#include "repositories/BillingRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "utils/Config.hpp"
#include "utils/ErrorResponse.hpp"
#include "utils/Time.hpp"

namespace Api {

void BillingController::listPackages(const HttpRequestPtr& req,
                                     std::function<void(const HttpResponsePtr&)>&& callback) {
    if (!require_billing_enabled(callback))
        return;
    API_REQUIRE_PRINCIPAL(req, callback, principal);
    with_repo_errors(callback, "billing listPackages", [&] {
        Repositories::PackageRepository repo;
        std::int64_t rate = 0, min_cents = 0, max_cents = 0;
        billing_limits(rate, min_cents, max_cents);
        callback(Response::ok({{"data", to_json_array(repo.list_active())},
                               {"credits_per_unit", rate},
                               {"min_amount_cents", min_cents},
                               {"max_amount_cents", max_cents}}));
    });
}

void BillingController::getWallet(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    if (!require_billing_enabled(callback))
        return;
    API_REQUIRE_PRINCIPAL(req, callback, principal);
    const auto page = parse_page_params(req, /*default_limit=*/20, /*max_limit=*/100);
    with_repo_errors(callback, "billing getWallet", [&] {
        const auto balance = Billing::balance_of(principal->subject);
        auto hist = Billing::history(principal->subject, page.limit, page.offset);
        json data = json::array();
        for (const auto& e : hist)
            data.push_back(public_wallet_entry(e));
        callback(Response::ok(
            {{"data", {{"balance", balance}, {"history", data}}}, {"limit", page.limit}, {"offset", page.offset}}));
    });
}

void BillingController::topup(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    if (!require_billing_enabled(callback))
        return;
    API_REQUIRE_PRINCIPAL(req, callback, principal);
    json body;
    if (!Validation::parse_body(req, body, callback))
        return;

    TopupPlan plan;
    if (!resolve_topup_plan(body, plan, callback))
        return;

    auto& cfg = Config::get();
    const std::string currency = cfg.get<std::string>("billing.currency", "BILLING_CURRENCY", "USD");
    const std::string return_url = cfg.get<std::string>("billing.paypal.return_url", "PAYPAL_RETURN_URL", "");
    const std::string cancel_url = cfg.get<std::string>("billing.paypal.cancel_url", "PAYPAL_CANCEL_URL", "");

    with_repo_errors(callback, "billing topup", [&] {
        // reference_id is PayPal-dashboard reconciliation only (see
        // PayPalClient::create_order's doc comment) — it is never read
        // back and never influences credited amounts.
        const std::string reference = "topup:" + principal->subject;
        auto order =
            Billing::PayPalClient::get().create_order(plan.amount_cents, currency, reference, return_url, cancel_url);

        Repositories::PaymentRepository payments;
        payments.create(principal->subject,
                        order.order_id,
                        plan.amount_cents,
                        currency,
                        plan.credits_expected,
                        plan.rate_snapshot,
                        plan.package_id);

        // The response deliberately carries the approve URL (+ order id,
        // needed to later call /capture) only — the client never sees or
        // supplies a credit count.
        callback(Response::created({{"data", {{"order_id", order.order_id}, {"approve_url", order.approve_url}}}}));
    });
}

void BillingController::capture(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    if (!require_billing_enabled(callback))
        return;
    API_REQUIRE_PRINCIPAL(req, callback, principal);
    json body;
    if (!Validation::parse_body(req, body, callback))
        return;
    Validation::Errors errs;
    Validation::require(errs, body, "order_id");
    if (!errs.any() && !body["order_id"].is_string())
        errs.add("order_id", "not_string", "order_id must be a string");
    if (errs.any()) {
        callback(Validation::response_400(errs));
        return;
    }
    const std::string order_id = body["order_id"].get<std::string>();

    // Set ONLY on the credit_capture path below, inside the guarded
    // lambda — consumed by the after-response block once the response is
    // already on the wire (the with_repo_errors overload with an
    // after_fn exists precisely for this shape — see HandlerSupport.hpp:
    // a side effect inside the guarded lambda that throws after
    // callback() fired would trigger a SECOND callback() via the catch
    // ladder).
    std::optional<Billing::CreditResult> capture_result;

    with_repo_errors(
        callback,
        "billing capture",
        [&] {
            Repositories::PaymentRepository payments;
            // Read the PRIMARY: a capture attempted moments after topup
            // (the common case — the buyer approves on PayPal and bounces
            // straight back) must not spuriously 404 because of replica
            // lag.
            auto payment = payments.find_by_order_id(order_id, /*from_primary=*/true);
            if (!payment) {
                callback(ErrorResponse::not_found("payment"));
                return;
            }
            // SECURITY: order_id comes straight from the request body —
            // an unauthorized order id must never reach
            // PayPalClient::capture_order or Billing::credit_capture.
            // Read the PRIMARY: this check is the only thing standing
            // between an attacker and someone else's wallet, so a lagging
            // replica must never make it fail open.
            auto owned = payments.find_owned(payment->id, principal->subject, /*from_primary=*/true);
            if (!owned) {
                // Same 404 whether the order doesn't exist or belongs to
                // someone else — no signal to a caller probing order ids.
                callback(ErrorResponse::not_found("payment"));
                return;
            }

            if (owned->status == Domain::PaymentStatus::kCaptured) {
                // Idempotent short-circuit: never re-issue a capture call
                // to PayPal for an order this service already captured —
                // a second real PayPal capture on an already-captured
                // order is an error on PayPal's side, not a no-op.
                const auto balance = Billing::balance_of(principal->subject, /*from_primary=*/true);
                callback(Response::ok({{"data", {{"credited", false}, {"balance", balance}, {"status", "captured"}}}}));
                return;
            }
            if (owned->status == Domain::PaymentStatus::kFailed || owned->status == Domain::PaymentStatus::kRefunded) {
                // A clean 409, no PayPal call — capturing an order that's
                // already failed or refunded on our side can only ever
                // error on PayPal's side too (or worse, be misleading).
                callback(ErrorResponse::conflict("payment_not_capturable",
                                                 "This payment is " + owned->status + " and cannot be captured"));
                return;
            }

            Billing::PayPalCapture cap;
            try {
                cap = Billing::PayPalClient::get().capture_order(order_id);
            } catch (const std::exception& e) {
                // PayPal's own "buyer hasn't approved this order yet"
                // error — a real, expected 4xx-shaped condition (a client
                // racing the approve redirect), not a server fault.
                if (std::string(e.what()).find("ORDER_NOT_APPROVED") != std::string::npos) {
                    callback(
                        ErrorResponse::conflict("order_not_approved", "This PayPal order has not been approved yet"));
                    return;
                }
                throw;  // anything else: with_repo_errors' outer catch maps it to 500.
            }

            if (cap.status != "COMPLETED") {
                // PayPal answers 2xx for PENDING (eCheck, fraud review)
                // and DECLINED captures too — crediting on either would
                // hand out credits for money that may never settle. Leave
                // the payment uncaptured (still created/approved) so the
                // webhook can resolve it once PayPal reaches a final
                // state.
                const auto balance = Billing::balance_of(principal->subject);
                callback(Response::ok(
                    {{"data",
                      {{"credited", false}, {"balance", balance}, {"status", cap.status}, {"pending", true}}}}));
                return;
            }

            auto result = Billing::credit_capture(order_id, cap.capture_id, cap.amount_cents, cap.currency);
            callback(Response::ok(
                {{"data", {{"credited", result.credited}, {"balance", result.balance}, {"status", "captured"}}}}));
            // Stash for the after-response block — never dispatch side
            // effects from inside this guarded lambda (see the doc
            // comment on capture_result above).
            capture_result = result;
        },
        [&] {
            // After-response side effects only: the response is already
            // sent (or, on any early-return/error path above,
            // capture_result is still empty and nothing runs at all).
            // `owned` (read inside the lambda, before the PayPal capture
            // call) was already verified to be neither captured/failed/
            // refunded, so was_failed_before=false is correct here: any
            // 'failed' status found now can only be a transition this
            // call itself just caused (modulo the rare documented race
            // with a concurrent webhook — see
            // dispatchFailedEmailIfJustTransitioned's doc comment).
            if (capture_result) {
                spdlog::info("billing capture: payment {} credited={} balance={}",
                             capture_result->payment_id,
                             capture_result->credited,
                             capture_result->balance);
                if (capture_result->credited)
                    dispatchReceiptEmail(*capture_result);
                else
                    dispatchFailedEmailIfJustTransitioned(*capture_result, /*was_failed_before=*/false);
            }
        });
}

void BillingController::paypalWebhook(const HttpRequestPtr& req,
                                      std::function<void(const HttpResponsePtr&)>&& callback) {
    if (!require_billing_enabled(callback))
        return;

    // RAW body, exactly as received — verified BEFORE any trusted parse.
    // See PayPalClient::verify_webhook_signature's doc comment: it does
    // its own json::parse of this same string to build PayPal's verify
    // request, but nothing upstream may re-serialize/re-derive it first.
    const std::string raw_body(req->body());
    const auto headers = collect_paypal_headers(req);

    bool verified = false;
    try {
        verified = Billing::PayPalClient::get().verify_webhook_signature(headers, raw_body);
    } catch (const std::exception& e) {
        // Transport/non-2xx from PayPal's OWN verify API — "we couldn't
        // ask PayPal", never conflated with "PayPal said no" (see
        // PayPalClient.hpp). 500 tells PayPal to retry later.
        spdlog::error("billing webhook: verify-webhook-signature API unreachable: {}", e.what());
        callback(ErrorResponse::internal_error());
        return;
    }
    if (!verified) {
        spdlog::warn("billing webhook: signature verification failed — rejecting, nothing credited/refunded");
        callback(ErrorResponse::unauthorized("invalid_signature"));
        return;
    }

    // Everything past this point parses/dispatches a signature-VALID but
    // still schema-UNTRUSTED body — PayPal signing a request proves only
    // that PayPal sent it, not that its shape matches what this handler
    // expects. The whole thing — including the two json::value() reads
    // right below (a signature-valid "id": 12345 would throw
    // nlohmann::json::type_error out of an unwrapped handler) — runs
    // inside ONE try/catch. See this method's doc comment in the header for
    // the full 200/401/5xx breakdown.
    try {
        json event = json::parse(raw_body);  // defensive re-parse; verify_webhook_signature() already required
                                             // valid JSON to return true.

        const std::string event_id = event.value("id", std::string());
        const std::string event_type = event.value("event_type", std::string());
        // Every received (signature-valid) event id, at info level — the
        // dedupe/debug trail, independent of whether this handler acts
        // on the event.
        spdlog::info("billing webhook: received event id={} type={}", event_id, event_type);

        auto respond_handled = [callback](bool handled) { callback(Response::ok({{"data", {{"handled", handled}}}})); };

        if (event_type == "PAYMENT.CAPTURE.COMPLETED") {
            handleCaptureCompleted(event, event_id, respond_handled);
        } else if (event_type == "PAYMENT.CAPTURE.REFUNDED") {
            handleCaptureRefunded(event, event_id, "refund", respond_handled);
        } else if (event_type == "PAYMENT.CAPTURE.REVERSED") {
            // PayPal reverses a payment capture (chargeback/dispute/risk
            // hold) — see handleCaptureRefunded's doc comment for why
            // this is routed into the exact same handler as a
            // merchant-initiated refund rather than a distinct path.
            handleCaptureRefunded(event, event_id, "reversal", respond_handled);
        } else {
            spdlog::info("billing webhook: ignoring unrelated event type '{}' (event {})", event_type, event_id);
            respond_handled(false);
        }
    } catch (const std::exception& e) {
        // Covers: malformed top-level shape (event_id/event_type reads
        // above), and anything handleCaptureRefunded lets propagate
        // (see its doc comment — a refund/reversal that "can't be
        // applied" is deliberately NOT swallowed into a 200 here).
        // handleCaptureCompleted, by contrast, still catches its own
        // failures locally and acks 200 — unaffected by this catch.
        spdlog::error("billing webhook: failed to process signature-verified event body: {}", e.what());
        callback(ErrorResponse::internal_error());
    }
}

bool BillingController::require_billing_enabled(const std::function<void(const HttpResponsePtr&)>& callback) {
    if (Core::billing_enabled())
        return true;
    callback(ErrorResponse::not_found("billing"));
    return false;
}

std::optional<Domain::User> BillingController::load_user_for_billing_email(const std::string& user_id) {
    try {
        Repositories::UserRepository users;
        auto u = users.find(user_id);
        if (!u)
            spdlog::warn("billing email: user {} not found — skipping", user_id);
        return u;
    } catch (const std::exception& e) {
        spdlog::warn("billing email: failed to load user {}: {}", user_id, e.what());
        return std::nullopt;
    }
}

std::string BillingController::package_title_for(const Domain::Payment& payment) {
    if (!payment.package_id)
        return "Custom top-up";
    try {
        Repositories::PackageRepository packages;
        auto pkg = packages.find(*payment.package_id);
        if (pkg)
            return pkg->title;
    } catch (const std::exception& e) {
        spdlog::warn("billing email: failed to load package {} for title: {}", *payment.package_id, e.what());
    }
    return "Custom top-up";
}

std::string BillingController::now_iso8601() {
    return Utils::Time::epoch_to_iso8601(Utils::Time::now_epoch_seconds());
}

std::string BillingController::friendly_failure_reason(const std::string& internal_reason) {
    if (internal_reason.find("mismatch") != std::string::npos)
        return "The payment amount didn't match your order, so it was declined. You were not charged.";
    return "We couldn't verify your payment, so it was declined. You were not charged.";
}

void BillingController::dispatchReceiptEmail(const Billing::CreditResult& result) {
    try {
        Repositories::PaymentRepository payments;
        auto payment = payments.find(result.payment_id, /*from_primary=*/true);
        if (!payment) {
            spdlog::warn("billing email: payment {} not found for receipt", result.payment_id);
            return;
        }
        auto user = load_user_for_billing_email(payment->user_id);
        if (!user)
            return;
        Email::BillingEmails::receipt(*user,
                                      package_title_for(*payment),
                                      payment->amount_cents,
                                      payment->currency,
                                      payment->credits_expected,
                                      result.balance,
                                      payment->id,
                                      now_iso8601());
    } catch (const std::exception& e) {
        spdlog::warn("billing email: failed to dispatch receipt for payment {}: {}", result.payment_id, e.what());
    }
}

void BillingController::dispatchFailedEmailIfJustTransitioned(const Billing::CreditResult& result,
                                                              bool was_failed_before) {
    if (was_failed_before)
        return;
    try {
        Repositories::PaymentRepository payments;
        auto payment = payments.find(result.payment_id, /*from_primary=*/true);
        if (!payment || payment->status != Domain::PaymentStatus::kFailed)
            return;  // credited=false but not a failed transition (e.g. a concurrent duplicate) — nothing to send.
        auto user = load_user_for_billing_email(payment->user_id);
        if (!user)
            return;
        Email::BillingEmails::failed(
            *user,
            payment->amount_cents,
            payment->currency,
            friendly_failure_reason(payment->failure_reason.value_or("payment could not be verified")),
            now_iso8601());
    } catch (const std::exception& e) {
        spdlog::warn(
            "billing email: failed to dispatch failed-payment email for payment {}: {}", result.payment_id, e.what());
    }
}

std::string BillingController::title_case_refund_kind(const std::string& kind_label) {
    return kind_label == "reversal" ? "Reversal" : "Refund";
}

std::optional<Domain::WalletEntry> BillingController::find_refund_ledger_entry(const std::string& user_id,
                                                                               const std::string& provider_refund_id) {
    for (const auto& e : Billing::history(user_id, /*limit=*/10, /*offset=*/0, /*from_primary=*/true)) {
        if (e.kind == Domain::WalletEntryKind::kRefund && e.reference == provider_refund_id)
            return e;
    }
    return std::nullopt;
}

void BillingController::dispatchRefundEmailIfApplied(const Billing::CreditResult& result,
                                                     const char* kind_label,
                                                     const std::string& provider_refund_id,
                                                     std::int64_t refunded_amount_cents,
                                                     const std::string& currency) {
    if (!result.credited)
        return;  // a redelivery of an already-seen refund id — never a new email.
    try {
        Repositories::PaymentRepository payments;
        auto payment = payments.find(result.payment_id, /*from_primary=*/true);
        if (!payment)
            return;
        auto entry = find_refund_ledger_entry(payment->user_id, provider_refund_id);
        if (!entry)
            return;  // durably recorded but skipped (insufficient balance / zero credits) — no debit happened.
        auto user = load_user_for_billing_email(payment->user_id);
        if (!user)
            return;
        Email::BillingEmails::refund(*user,
                                     title_case_refund_kind(kind_label),
                                     refunded_amount_cents,
                                     currency,
                                     /*credits_deducted=*/-entry->delta_credits,
                                     result.balance,
                                     payment->id,
                                     now_iso8601());
    } catch (const std::exception& e) {
        spdlog::warn("billing email: failed to dispatch refund email for payment {}: {}", result.payment_id, e.what());
    }
}

std::map<std::string, std::string> BillingController::collect_paypal_headers(const HttpRequestPtr& req) {
    std::map<std::string, std::string> h;
    auto add = [&](const char* name) {
        std::string v = req->getHeader(name);
        if (!v.empty())
            h[name] = v;
    };
    add("Paypal-Auth-Algo");
    add("Paypal-Cert-Url");
    add("Paypal-Transmission-Id");
    add("Paypal-Transmission-Sig");
    add("Paypal-Transmission-Time");
    return h;
}

std::string BillingController::extract_capture_id_from_links(const json& resource) {
    if (!resource.contains("links") || !resource["links"].is_array())
        return {};
    static constexpr std::string_view kMarker = "/captures/";
    for (const auto& link : resource["links"]) {
        if (!link.is_object() || link.value("rel", std::string()) != "up")
            continue;
        const std::string href = link.value("href", std::string());
        const auto marker_pos = href.rfind(kMarker);
        if (marker_pos == std::string::npos)
            continue;
        std::string id = href.substr(marker_pos + kMarker.size());
        const auto cut = id.find_first_of("?#");
        if (cut != std::string::npos)
            id = id.substr(0, cut);
        while (!id.empty() && id.back() == '/')
            id.pop_back();
        if (!id.empty())
            return id;
    }
    return {};
}

void BillingController::handleCaptureCompleted(const json& event,
                                               const std::string& event_id,
                                               std::function<void(bool)> respond) {
    const json resource = event.value("resource", json::object());
    std::string order_id, capture_id, currency;
    std::int64_t amount_cents = 0;
    try {
        order_id = resource.at("supplementary_data").at("related_ids").at("order_id").get<std::string>();
        capture_id = resource.at("id").get<std::string>();
        currency = resource.at("amount").at("currency_code").get<std::string>();
        amount_cents = Billing::detail::parse_decimal_to_cents(resource.at("amount").at("value").get<std::string>());
    } catch (const std::exception& e) {
        spdlog::error(
            "billing webhook: malformed PAYMENT.CAPTURE.COMPLETED resource (event {}): {}", event_id, e.what());
        respond(false);
        return;
    }

    // Read-only, best-effort: used ONLY to tell "this call just caused a
    // failed transition" apart from "already failed before this call"
    // for the failed-email dispatch below — see
    // dispatchFailedEmailIfJustTransitioned's doc comment. A lookup
    // failure here must never block the actual credit, so it defaults to
    // the safe direction (assume already failed -> skip the email rather
    // than risk a duplicate).
    bool was_failed_before = true;
    try {
        Repositories::PaymentRepository pre_read;
        auto existing = pre_read.find_by_order_id(order_id, /*from_primary=*/true);
        was_failed_before = existing && existing->status == Domain::PaymentStatus::kFailed;
    } catch (const std::exception& e) {
        spdlog::warn("billing webhook: pre-read for email dispatch failed for order {}: {}", order_id, e.what());
    }

    try {
        auto result = Billing::credit_capture(order_id, capture_id, amount_cents, currency);
        spdlog::info("billing webhook: capture {} order {} (event {}) — credited={} balance={}",
                     capture_id,
                     order_id,
                     event_id,
                     result.credited,
                     result.balance);
        // Outside credit_capture's own (already-committed) transaction —
        // see the "Billing email dispatch" note on the header's helper
        // declarations.
        if (result.credited)
            dispatchReceiptEmail(result);
        else
            dispatchFailedEmailIfJustTransitioned(result, was_failed_before);
    } catch (const std::exception& e) {
        // An unknown order id, a capture id already claimed by a
        // different order, or any other repository-layer anomaly: log
        // loudly for manual reconciliation but still ack 200 — retrying
        // this exact event can never resolve a structural mismatch, and
        // a non-2xx here just means PayPal hammers this endpoint for
        // days over a condition that will never change on its own.
        spdlog::error("billing webhook: credit_capture failed for order {} capture {} (event {}): {}",
                      order_id,
                      capture_id,
                      event_id,
                      e.what());
    }
    respond(true);
}

void BillingController::handleCaptureRefunded(const json& event,
                                              const std::string& event_id,
                                              const char* kind_label,  // "refund" or "reversal" — logging only
                                              std::function<void(bool)> respond) {
    const json resource = event.value("resource", json::object());
    const std::string refund_id = resource.at("id").get<std::string>();
    // currency_code is extracted for logging/traceability only —
    // refund_capture() takes no currency parameter: it always debits
    // against the ORIGINAL payment's own currency (fixed at
    // credit_capture time), and PayPal never refunds/reverses a capture
    // in a different currency than it was captured in.
    const std::string currency = resource.at("amount").at("currency_code").get<std::string>();
    const std::int64_t amount_cents =
        Billing::detail::parse_decimal_to_cents(resource.at("amount").at("value").get<std::string>());
    const std::string capture_id = extract_capture_id_from_links(resource);
    if (capture_id.empty()) {
        spdlog::error(
            "billing webhook: PAYMENT.CAPTURE {} {} (event {}) has no resolvable capture id from its 'up' "
            "link — will retry (5xx) rather than drop it",
            kind_label,
            refund_id,
            event_id);
        throw std::runtime_error("billing webhook: unresolvable capture id for " + std::string(kind_label) + " " +
                                 refund_id);
    }

    try {
        auto result = Billing::refund_capture(capture_id, refund_id, amount_cents);
        spdlog::info("billing webhook: {} {} capture {} currency {} (event {}) — newly recorded={} balance={}",
                     kind_label,
                     refund_id,
                     capture_id,
                     currency,
                     event_id,
                     result.credited,
                     result.balance);
        // Outside refund_capture's own (already-committed) transaction —
        // see the "Billing email dispatch" note on the header's helper
        // declarations. Only sends when the ledger shows this call actually
        // debited the wallet (see dispatchRefundEmailIfApplied's doc
        // comment).
        dispatchRefundEmailIfApplied(result, kind_label, refund_id, amount_cents, currency);
    } catch (const Repositories::ValidationError& e) {
        // InvalidRefundAmount: PayPal's own reported amount is out of
        // range, or pushes the cumulative refunded+reversed total for
        // this capture past payments.amount_cents (refund_capture's own
        // guard — see Wallet.hpp). THIS is what keeps a REFUNDED
        // followed by a REVERSED on the same capture (distinct ids) from
        // ever double-debiting past the original payment amount: the
        // second one lands here and writes nothing at all, not even a
        // billing_refunds row. Unlike an unresolvable capture id or an
        // unknown order, this is a structural mismatch a retry can never
        // resolve, so — deliberately unlike everything else this
        // function can fail on — it's caught HERE and acked 200 rather
        // than left to propagate into paypalWebhook's 5xx catch.
        spdlog::error("billing webhook: {} {} capture {} (event {}) refused by refund_capture: {}",
                      kind_label,
                      refund_id,
                      capture_id,
                      event_id,
                      e.what());
    }
    // Anything else refund_capture throws (notably
    // Repositories::PaymentNotFound) is NOT caught here — it propagates
    // to paypalWebhook's outer try/catch, which answers 5xx. See this
    // function's doc comment in the header for why.
    respond(true);
}

json BillingController::public_wallet_entry(const Domain::WalletEntry& e) {
    return json{
        {"id", e.id},
        {"user_id", e.user_id},
        {"delta_credits", e.delta_credits},
        {"kind", e.kind},
        {"reference", e.reference},
        {"note", e.note},
        {"created_at", e.created_at},
    };
}

void BillingController::billing_limits(std::int64_t& rate, std::int64_t& min_cents, std::int64_t& max_cents) {
    try {
        Repositories::BillingSettingsRepository settings;
        auto s = settings.get();
        rate = s.credits_per_unit;
        min_cents = s.min_amount_cents;
        max_cents = s.max_amount_cents;
        return;
    } catch (const std::exception& e) {
        spdlog::warn("billing_limits: billing_settings row unavailable, falling back to config: {}", e.what());
    }
    auto& cfg = Config::get();
    rate = static_cast<std::int64_t>(cfg.get<long>("billing.credits_per_unit", "BILLING_CREDITS_PER_UNIT", 100));
    min_cents = static_cast<std::int64_t>(cfg.get<long>("billing.min_amount_cents", "BILLING_MIN_AMOUNT_CENTS", 100));
    max_cents =
        static_cast<std::int64_t>(cfg.get<long>("billing.max_amount_cents", "BILLING_MAX_AMOUNT_CENTS", 100000));
}

bool BillingController::resolve_topup_plan(const json& body,
                                           TopupPlan& out,
                                           const std::function<void(const HttpResponsePtr&)>& callback) {
    std::int64_t min_cents = 0, max_cents = 0;
    billing_limits(out.rate_snapshot, min_cents, max_cents);

    const bool has_package =
        body.contains("package_id") && body["package_id"].is_string() && !body["package_id"].get<std::string>().empty();
    const bool has_amount = body.contains("amount_cents") && !body["amount_cents"].is_null();

    if (has_package == has_amount) {
        callback(
            ErrorResponse::bad_request("invalid_topup_request", "Provide exactly one of package_id or amount_cents"));
        return false;
    }

    if (has_package) {
        const std::string package_id = body["package_id"].get<std::string>();
        if (!is_valid_uuid(package_id)) {
            callback(ErrorResponse::bad_request("invalid_uuid", "package_id is not a valid UUID"));
            return false;
        }
        Repositories::PackageRepository packages;
        auto pkg = packages.find(package_id);
        if (!pkg || !pkg->active) {
            callback(ErrorResponse::not_found("billing_package"));
            return false;
        }
        out.package_id = pkg->id;
        out.amount_cents = pkg->amount_cents;
        // Frozen exactly as priced by the admin catalogue — never
        // re-derived from the current per-unit rate.
        out.credits_expected = pkg->credits;
    } else {
        if (!body["amount_cents"].is_number_integer()) {
            callback(ErrorResponse::bad_request("not_integer", "amount_cents must be an integer"));
            return false;
        }
        out.amount_cents = body["amount_cents"].get<std::int64_t>();
        // Integer math only — see Billing::refund_capture's identical rule.
        out.credits_expected = (out.amount_cents * out.rate_snapshot) / 100;
    }

    // Guard BEFORE the PayPal order is ever created (topup() calls
    // create_order right after this returns true): integer division can
    // floor a small-but-in-range amount_cents to 0 credits if an admin
    // sets rate_snapshot (or a package's credits) low enough — e.g.
    // min_amount_cents * credits_per_unit < 100. `payments.credits_expected`
    // has a `CHECK (credits_expected > 0)`, so letting this through would
    // create a live PayPal order and then hit that constraint on
    // PaymentRepository::create as a bare 500 — with the order already
    // placed at PayPal and nothing in our own DB pointing back at it (an
    // orphan). Catching it here means the order is never created at all.
    if (out.credits_expected <= 0) {
        callback(ErrorResponse::bad_request("credits_too_small", "amount too small for the current rate"));
        return false;
    }

    if (out.amount_cents <= 0 || out.amount_cents < min_cents || out.amount_cents > max_cents) {
        const char* code = has_package ? "package_price_out_of_range" : "amount_out_of_range";
        callback(ErrorResponse::bad_request(
            code, "amount_cents must be between " + std::to_string(min_cents) + " and " + std::to_string(max_cents)));
        return false;
    }
    return true;
}

}  // namespace Api
