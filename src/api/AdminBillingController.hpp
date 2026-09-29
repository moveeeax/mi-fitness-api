/**
 * @file AdminBillingController.hpp
 * @brief Admin billing API: sale-package CRUD, the runtime rate/bounds
 *        settings, the payments ledger view, manual wallet adjustments, and
 *        the business-metrics aggregate.
 * @details Every handler: module guard (require_billing_enabled) →
 *          API_REQUIRE_ADMIN → Api::with_repo_errors — same shape as
 *          BillingController and AdminController. Every mutation (package
 *          create/update/delete, settings update, manual adjust) writes a
 *          Security::Audit::record() row AFTER its own write succeeds,
 *          mirroring AdminController's user/role CRUD exactly.
 *
 * Declarations only — the handler bodies live in AdminBillingController.cpp
 * (compiled once into app_core; ADR 0003 as amended 2026-08-22). The route
 * macros (ADD_METHOD_TO) must stay in this header: Drogon's METHOD_LIST
 * registration is part of the class definition, and
 * scripts/check-routes-registered.sh greps the src/api headers for them.
 *
 * Money-critical notes:
 *   - POST .../users/{id}/adjust is a thin wrapper over Billing::adjust
 *     (src/billing/Wallet.hpp) — this controller NEVER writes wallet_entries
 *     / wallet_balances directly. `note` must be non-empty (validated here;
 *     Billing::adjust itself does not enforce that) and `admin_id` is always
 *     the authenticated caller's own subject, never a client-supplied value.
 *     Optional `notify` (bool, default false) sends the target user a
 *     best-effort Email::BillingEmails::adjustment() notice AFTER the
 *     adjust + audit write both succeeded — dispatched from with_repo_errors'
 *     after_fn, never inside the guarded lambda (see HandlerSupport.hpp's
 *     incident note), so a throwing dispatch can never corrupt the money
 *     path or double-answer the request.
 *   - PUT .../settings changes the rate/bounds `billing_settings` row
 *     (migration 008) that BillingController::billing_limits() reads. This
 *     can NEVER retroactively change an in-flight/already-created payment:
 *     `payments.credits_expected` / `rate_snapshot` are frozen at creation
 *     time (untouched by this controller) — only a NEW top-up computed after
 *     the change observes it.
 *   - Amounts/credits/rate are integers end to end; no floating point.
 */

#pragma once

#include <functional>
#include <optional>
#include <string>

#include <drogon/HttpController.h>

#include "domain/User.hpp"

namespace Api {

using namespace drogon;

class AdminBillingController : public HttpController<AdminBillingController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AdminBillingController::listPayments, "/api/v1/admin/billing/payments", Get);
    ADD_METHOD_TO(AdminBillingController::listPackages, "/api/v1/admin/billing/packages", Get);
    ADD_METHOD_TO(AdminBillingController::createPackage, "/api/v1/admin/billing/packages", Post);
    ADD_METHOD_TO(AdminBillingController::updatePackage, "/api/v1/admin/billing/packages/{1}", Patch);
    ADD_METHOD_TO(AdminBillingController::deletePackage, "/api/v1/admin/billing/packages/{1}", Delete);
    ADD_METHOD_TO(AdminBillingController::getSettings, "/api/v1/admin/billing/settings", Get);
    ADD_METHOD_TO(AdminBillingController::updateSettings, "/api/v1/admin/billing/settings", Put);
    ADD_METHOD_TO(AdminBillingController::adjustWallet, "/api/v1/admin/billing/users/{1}/adjust", Post);
    ADD_METHOD_TO(AdminBillingController::metrics, "/api/v1/admin/billing/metrics", Get);
    METHOD_LIST_END

    // ── GET /api/v1/admin/billing/payments ──────────────────────────────────
    // Paged, newest first. Optional ?status= and ?user_id= filters, combined
    // with AND (both may be given at once). See BillingRepository.hpp's
    // PaymentRepository::list_filtered for the parameter-bound SQL.
    void listPayments(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

    // ── GET /api/v1/admin/billing/packages ──────────────────────────────────
    // Unlike the user-facing GET /billing/packages, this returns EVERY
    // package (active and inactive) so the admin catalogue view can manage
    // both — PackageRepository::list_active() is deliberately not used here.
    void listPackages(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

    // ── POST /api/v1/admin/billing/packages ─────────────────────────────────
    void createPackage(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

    // ── PATCH /api/v1/admin/billing/packages/{id} ───────────────────────────
    void updatePackage(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback,
                       const std::string& id);

    // ── DELETE /api/v1/admin/billing/packages/{id} ──────────────────────────
    void deletePackage(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback,
                       const std::string& id);

    // ── GET /api/v1/admin/billing/settings ──────────────────────────────────
    void getSettings(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

    // ── PUT /api/v1/admin/billing/settings ──────────────────────────────────
    // A full replace (not a partial patch): all three fields are required,
    // so the resulting row is never a mix of an old and a new value the
    // caller never actually agreed to.
    void updateSettings(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

    // ── POST /api/v1/admin/billing/users/{id}/adjust ────────────────────────
    // Manual wallet credit/debit. Routes through Billing::adjust — this
    // controller never touches wallet_entries/wallet_balances directly.
    // `note` is mandatory (non-empty): Billing::adjust itself does not
    // enforce that, so it's validated here before any DB write. `admin_id`
    // is always the authenticated caller's own subject — never taken from
    // the request body — so wallet_entries.created_by can't be spoofed.
    // Optional `notify` (bool, default false): when true and the adjust +
    // audit write both succeed, sends the target user a best-effort
    // Email::BillingEmails::adjustment() notice carrying `note` as the
    // reason. Dispatch happens in with_repo_errors' after_fn — the response
    // is already on the wire, and a dispatch-helper exception can never be
    // translated into a second callback() (see HandlerSupport.hpp's
    // incident note; same shape as BillingController::capture).
    void adjustWallet(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback,
                      const std::string& id);

    // ── GET /api/v1/admin/billing/metrics ───────────────────────────────────
    // Business-metrics dashboard aggregate: revenue/count/avg (captured
    // payments), conversion (captured vs created), applied refunds, the
    // all-time outstanding wallet liability, a gap-free time series, and top
    // packages/users — all in one read-only round trip. See
    // BillingMetricsRepository.hpp for the exact SQL and window semantics.
    void metrics(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

private:
    /// Module gate — same contract as BillingController::require_billing_enabled
    /// (kept controller-local because Guards.hpp has no generic per-module
    /// variant).
    static bool require_billing_enabled(const std::function<void(const HttpResponsePtr&)>& callback);

    /// Acting admin's principal subject for the audit trail ("" when auth off).
    static std::string actor_of(const HttpRequestPtr& req);

    /// Loads the target user for the optional adjustment-notice email. A
    /// missing user or a repo failure is logged and treated as "skip this
    /// email" — mirrors BillingController::load_user_for_billing_email;
    /// never throws into the money path.
    static std::optional<Domain::User> load_user_for_adjustment_email(const std::string& user_id);
};

}  // namespace Api
