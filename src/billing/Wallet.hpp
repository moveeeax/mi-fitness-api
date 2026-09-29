/**
 * @file Wallet.hpp
 * @brief The credit wallet's crediting/refund/adjustment/spend service — the
 *        ONLY code in this codebase allowed to write `wallet_entries` or
 *        `wallet_balances`. Everything else (the top-up endpoint, the
 *        provider webhook, the admin adjust endpoint — the module's HTTP
 *        layer — and a fork's charging flow) calls into these functions
 *        instead of touching the ledger tables directly.
 *
 * Declarations only — the bodies live in Wallet.cpp (compiled once into
 * app_core; ADR 0003 as amended 2026-08-22).
 *
 * Money invariants enforced here (see migrations/007_billing.sql):
 *   - every amount is a BIGINT (cents / credits) — no floating point;
 *   - `wallet_entries` is append-only: this module only ever INSERTs into it;
 *   - `wallet_balances` is a cache kept in the SAME transaction as the
 *     ledger insert that changed it;
 *   - `credit_capture` moves `payments.credits_expected` — frozen at order
 *     creation — never a value re-derived from the current rate;
 *   - `credit_capture` is idempotent on `provider_capture_id`: the guarded
 *     UPDATE (`... WHERE provider_capture_id IS NULL`) is the structural
 *     race guard, backed by the UNIQUE index in the migration;
 *   - `spend` is idempotent on its caller-chosen `reference` (partial UNIQUE
 *     index over kind='spend' rows, migration 009) and refuses — rather than
 *     records — an overdraw: see its own docs below;
 *   - `refund_capture` is idempotent on the provider refund id, via a durable
 *     `billing_refunds` row written on EVERY call (applied or skipped) — NOT
 *     on payments.status and NOT on a wallet_entries row, since a skipped
 *     refund (see below) deliberately writes neither of those but must still
 *     be remembered so a redelivery can't re-decide (or worse, re-apply
 *     later once the balance happens to recover).
 *
 * `wallet_balances` upserts NEVER use a self-referencing SQL expression
 * (`SET credits = wallet_balances.credits + EXCLUDED.credits`) — every write
 * here locks the row first (`SELECT ... FOR UPDATE`), computes the new total
 * in C++ from that locked read, and upserts with `SET credits =
 * EXCLUDED.credits` against the explicit computed value. An earlier version
 * of this module used the self-referencing form; CI proved it does not
 * reliably read the pre-existing row inside its own ON CONFLICT DO UPDATE —
 * every write after the first for a given user silently discarded the true
 * prior balance and computed as though it were 0. Invisible for a positive
 * delta (a wrong-but-non-negative result never trips `CHECK (credits >=
 * 0)`); fatal for a negative one (a correctly-sufficient refund/debit
 * against a real balance still computed as negative and 500'd). Root cause
 * never conclusively found — forensics live in the site fork's commit
 * b676430; this module is the primary source for the corresponding "Don'ts"
 * entry in CLAUDE.md. See refund_capture's docs for the full diagnosis.
 *
 * Every write also INSERTs the row (`VALUES ($1, 0) ON CONFLICT (user_id) DO
 * NOTHING`) BEFORE the `SELECT ... FOR UPDATE`, then closes with a plain
 * `UPDATE ... WHERE user_id = $1` instead of an upsert. `SELECT ... FOR
 * UPDATE` cannot lock a row that does not exist yet, so without this a
 * brand-new user's first pair of concurrent balance-changing writes (no
 * `wallet_balances` row) would both read `current = 0` with no lock held,
 * and the second writer's result would silently clobber the first —
 * `wallet_balances.credits` disagreeing with `SUM(wallet_entries.delta_credits)`
 * even though the ledger itself stays correct (a lost-update on the cache,
 * not an over-credit). Materializing the row first guarantees the `FOR
 * UPDATE` always has something real to hold, so the second writer blocks
 * and reads the first writer's committed total instead of racing it.
 *
 * `CreditResult::credited` means "this call durably recorded something new"
 * — false only for the idempotent no-op (an already-seen refund id, or an
 * already-captured order). It does NOT mean "the wallet balance moved": a
 * skipped refund (insufficient balance, or a sub-unit amount that converts
 * to 0 credits at the payment's rate) still returns credited=true, because
 * the attempt itself is now durably recorded in `billing_refunds` — see
 * refund_capture's docs below. Callers: branch on `credited` to mean "not
 * a duplicate", and inspect the payment/refund row separately for "did the
 * wallet actually move".
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "domain/Billing.hpp"
#include "repositories/RepoErrors.hpp"

namespace Billing {

/**
 * @brief → 409. A manual `adjust()` whose negative delta would drive the
 *        wallet below zero. Distinct from a refund going negative: an admin
 *        adjustment hasn't already moved real money, so it is refused up
 *        front instead of applied-with-a-warning.
 */
struct InsufficientBalance : Repositories::ConflictError {
    InsufficientBalance()
        : Repositories::ConflictError("insufficient_balance",
                                      "This adjustment would drive the wallet balance negative") {}
};

/**
 * @brief → 409. A `spend()` whose amount exceeds the wallet's current
 *        balance. Refused up front in C++ under the balance-row lock —
 *        nothing is written to the ledger at all (the wallet_balances
 *        CHECK (credits >= 0) is the second belt, not the primary check:
 *        a money refusal must surface as a clear 409, never a raw 23514).
 *        Distinct from InsufficientBalance (adjust's refusal) so callers
 *        and clients can tell an admin correction apart from a user-facing
 *        "not enough credits".
 */
struct InsufficientCredits : Repositories::ConflictError {
    InsufficientCredits()
        : Repositories::ConflictError("insufficient_credits",
                                      "The wallet does not hold enough credits for this spend") {}
};

/**
 * @brief → 400. `spend()` refuses a non-positive amount outright instead of
 *        letting delta_credits=0 trip the wallet_entries CHECK
 *        (delta_credits <> 0), or a negative "spend" silently CREDIT the
 *        wallet.
 */
struct InvalidSpendAmount : Repositories::ValidationError {
    InvalidSpendAmount() : Repositories::ValidationError("invalid_spend_amount", "credits must be > 0") {}
};

/**
 * @brief → 400. `spend()` requires a non-empty @p reference: it is the
 *        idempotency key (see migration 009's partial unique index), and an
 *        empty one would make every retried no-reference spend collide with
 *        every other — refused before anything is written.
 */
struct MissingSpendReference : Repositories::ValidationError {
    MissingSpendReference()
        : Repositories::ValidationError("missing_spend_reference",
                                        "reference must be a non-empty idempotency key for this spend") {}
};

/**
 * @brief → 409. A spend @p reference that is already recorded against a
 *        DIFFERENT user's wallet. A same-user retry never reaches this path
 *        — it lands in the ordinary idempotent no-op branch instead.
 *        Pathological (references are caller-scoped by construction), but —
 *        same reasoning as DuplicateCaptureId — money code must not let a
 *        constraint violation surface as a bare 500.
 */
struct DuplicateSpendReference : Repositories::ConflictError {
    DuplicateSpendReference()
        : Repositories::ConflictError("spend_reference_conflict",
                                      "This spend reference is already recorded against a different user") {}
};

/**
 * @brief → 409. `provider_capture_id` is globally UNIQUE across `payments`
 *        (the structural double-credit guard). This fires only if a capture
 *        id that already belongs to a DIFFERENT order is presented for this
 *        one — a same-order retry never reaches this path, it lands in the
 *        ordinary "already captured" idempotent branch instead. Pathological
 *        (provider capture ids are unique by construction), but money code
 *        must not let a constraint violation surface as a bare 500.
 */
struct DuplicateCaptureId : Repositories::ConflictError {
    DuplicateCaptureId()
        : Repositories::ConflictError("capture_id_conflict",
                                      "This capture id is already recorded against a different payment") {}
};

/**
 * @brief → 400. `refund_capture` refuses a nonsensical, over-large, or
 *        cumulatively-over-large refund amount up front — nothing is
 *        written at all (not even a `billing_refunds` row), unlike the
 *        insufficient-balance/zero-credit cases which still record the
 *        attempt.
 */
struct InvalidRefundAmount : Repositories::ValidationError {
    InvalidRefundAmount()
        : Repositories::ValidationError(
              "invalid_refund_amount",
              "refunded_amount_cents must be > 0, <= the payment's amount_cents, and the running total of all "
              "refunds for this payment must not exceed it") {}
};

/**
 * @brief → 400. `adjust()` refuses a no-op delta outright instead of letting
 *        it trip the wallet_entries CHECK (delta_credits <> 0) and surface
 *        as a confusing "insufficient_balance".
 */
struct ZeroAdjustment : Repositories::ValidationError {
    ZeroAdjustment() : Repositories::ValidationError("zero_adjustment", "delta_credits must not be zero") {}
};

/// → 404. `adjust()`'s user_id is a syntactically valid UUID that doesn't
/// reference an existing user (wallet_entries.user_id FK).
struct UnknownUser : Repositories::NotFoundError {
    UnknownUser() : Repositories::NotFoundError("user") {}
};

/// → 400. `adjust()`'s user_id is not a syntactically valid UUID at all —
/// distinct from UnknownUser.
struct MalformedUserId : Repositories::ValidationError {
    MalformedUserId() : Repositories::ValidationError("invalid_user_id", "user_id is not a valid UUID") {}
};

/// → 404. `adjust()`'s admin_id is a syntactically valid UUID that doesn't
/// reference an existing user (wallet_entries.created_by FK). Distinct from
/// UnknownUser: the same insert has TWO FK columns, and a bare SQLSTATE from
/// that insert can't say which one was bad — see adjust()'s pre-checks.
struct UnknownAdmin : Repositories::NotFoundError {
    UnknownAdmin() : Repositories::NotFoundError("admin") {}
};

/// → 400. `adjust()`'s admin_id is not a syntactically valid UUID at all.
struct MalformedAdminId : Repositories::ValidationError {
    MalformedAdminId() : Repositories::ValidationError("invalid_admin_id", "admin_id is not a valid UUID") {}
};

struct CreditResult {
    bool credited;
    std::int64_t balance;
    std::string payment_id;
};

/**
 * @brief Read the cached balance. @p from_primary forces the primary
 *        instead of a (possibly lagging) replica — pass true right after a
 *        write in the same request (e.g. immediately after credit_capture)
 *        so the caller never sees a stale pre-write balance.
 */
std::int64_t balance_of(const std::string& user_id, bool from_primary = false);

/// Paged ledger history, newest first. @p from_primary — see balance_of().
std::vector<Domain::WalletEntry> history(const std::string& user_id, int limit, int offset, bool from_primary = false);

/**
 * @brief Idempotent capture-and-credit. Called from both the return-flow
 *        capture endpoint and the webhook (the module's HTTP layer) — both
 *        funnel into this one function so there is exactly one place that
 *        can ever write a `topup` ledger row.
 *
 * ONE transaction: the guarded status/capture-id UPDATE, the amount+currency
 * check, the ledger insert and the balance upsert all commit or roll back
 * together. The balance upsert locks the wallet_balances row first
 * (`SELECT ... FOR UPDATE`) and computes the new total in C++ — see the
 * file-level note on why it never uses a self-referencing SQL expression.
 *
 * @throws Repositories::PaymentNotFound if @p provider_order_id is unknown.
 * @throws DuplicateCaptureId if @p provider_capture_id already belongs to a
 *         different payment (SQLSTATE 23505 on the UNIQUE index).
 */
CreditResult credit_capture(const std::string& provider_order_id,
                            const std::string& provider_capture_id,
                            std::int64_t captured_amount_cents,
                            const std::string& captured_currency);

/**
 * @brief Idempotent refund. Converts @p refunded_amount_cents to credits by
 *        prorating the payment's frozen `credits_expected` —
 *        `credits_expected * refunded_amount_cents / amount_cents` (integer
 *        division — no floating point) — and writes a negative `refund`
 *        ledger entry IF the conversion and the current balance allow it.
 *
 *        Deliberately NOT `refunded_amount_cents * rate_snapshot / 100`: a
 *        package sale's `credits_expected` is priced independently of
 *        `rate_snapshot` (a bonus/discount package hands out more or fewer
 *        credits than the generic per-unit rate would imply — see the
 *        top-up flow), so a rate_snapshot-based conversion silently
 *        disagrees with what was actually credited. Prorating off
 *        `credits_expected` instead is exact for a full refund by
 *        construction — `(X * Y) / Y == X` for any nonzero Y, so
 *        `refunded_amount_cents == amount_cents` always yields exactly
 *        `credits_expected` back, whether the payment came from a package or
 *        a custom amount — and proportional (floored) for a partial one.
 *        `rate_snapshot` is retained on `payments` purely as an informational
 *        record of the per-unit rate in effect at purchase time; nothing in
 *        this module reads it anymore.
 *
 * ONE transaction. The durable idempotency marker is a `billing_refunds` row
 * (migrations/007_billing.sql), written on EVERY non-duplicate call,
 * whatever the outcome:
 *
 *   1. Look up the payment by @p provider_capture_id — `FOR UPDATE`. This is
 *      what makes the aggregate check in step 3 race-safe: TWO concurrent
 *      refunds against the SAME payment (distinct refund ids, e.g. 600 and
 *      500 on a 1000-cent payment) now serialize on this row instead of
 *      both reading `already_refunded=0` and both passing. It also gives
 *      this function the same lock ORDER as credit_capture — payments
 *      first, wallet_balances second (see step 4) — so the two functions
 *      can never deadlock against each other over the same payment/user.
 *   2. Idempotency FIRST: if `billing_refunds` already has a row for
 *      @p provider_refund_id, this is a redelivery — return the existing
 *      state, write nothing. This is checked BEFORE the amount validation
 *      and BEFORE the sufficiency decision below, and it is why a
 *      redelivered refund that was previously skipped (insufficient balance,
 *      or a zero-credit conversion) stays skipped forever, even if the
 *      wallet balance later recovers enough to afford it — an earlier
 *      wallet_entries-based idempotency check could not do this, because a
 *      skipped refund never wrote a wallet_entries row at all.
 *   3. Validate @p refunded_amount_cents: must be `> 0` and
 *      `<= payments.amount_cents`, AND the running total of every
 *      `billing_refunds` row for this payment (applied or skipped — the
 *      provider's own count of what it refunded, not just what the wallet
 *      could reflect) plus this one must not exceed `payments.amount_cents`.
 *      The payments row lock from step 1 makes this sum trustworthy even
 *      under concurrent refund attempts. Either violation throws
 *      InvalidRefundAmount; the whole transaction rolls back, nothing is
 *      written — not even a `billing_refunds` row, since this refund was
 *      never a real attempt against this payment.
 *   4. Convert to credits (`credits_expected * refunded_amount_cents /
 *      amount_cents`, integer division — see the function-level note above
 *      on why this prorates off `credits_expected`, not `rate_snapshot`).
 *      `SELECT credits FROM wallet_balances ... FOR
 *      UPDATE` locks the user's balance row ONCE, up front, whatever the
 *      outcome — its value seeds the response in every branch below, and is
 *      also the sufficiency-decision input, so there is exactly one query
 *      here rather than a second one only some branches need. Two ways the
 *      credit application can be skipped, NEITHER of which may ever let the
 *      wallet_entries/wallet_balances CHECK constraints fire (that's what
 *      silently 500'd before):
 *        - `refund_credits == 0`: the amount is too small to represent at
 *          this payment's credits_expected/amount_cents ratio (e.g. 10
 *          credits for a 1000-cent payment, refund of 1-99 cents floors to
 *          0). The `wallet_entries.delta_credits <> 0` CHECK would otherwise
 *          reject the insert outright. Outcome: `skipped_zero_credits`.
 *        - the locked balance can't cover it
 *          (`current_balance - refund_credits < 0`), decided in C++ BEFORE
 *          any write, so `wallet_balances.CHECK (credits >= 0)` is never
 *          actually exercised. Outcome: `skipped_insufficient`.
 *      Either way: log at `error` for manual reconciliation, write the
 *      `billing_refunds` row with that outcome and `credits_deducted = 0`,
 *      skip the ledger insert entirely. Otherwise: ledger insert + balance
 *      upsert (the NEW total — `new_balance - refund_credits` — computed
 *      from the locked read above and set explicitly, never via a
 *      self-referencing SQL expression), outcome `applied`,
 *      `credits_deducted = refund_credits`.
 *   5. `payments.status` becomes `refunded` when the CUMULATIVE total of
 *      every refund against this payment (steps 3's running total,
 *      including this one) reaches `payments.amount_cents` AND the payment
 *      is currently `captured` — so a payment fully refunded across several
 *      partials (e.g. 400 then 600 on a 1000-cent payment) DOES end up
 *      `refunded` once the last one lands, not just a single call whose own
 *      amount happens to equal the full total. This fires regardless of the
 *      ledger outcome above (a full-by-now refund is a provider-side fact
 *      independent of whether the wallet could reflect every part of it),
 *      but a refund that leaves the cumulative total short of the full
 *      amount never touches `payments.status`, and a refund against a
 *      payment that isn't `captured` (e.g. `failed`) can't accidentally
 *      flip it either.
 *
 * All of the above commits atomically in this one transaction.
 *
 * A concurrent duplicate (two redeliveries of the same refund id racing each
 * other) loses on `billing_refunds.provider_refund_id`'s UNIQUE constraint
 * (SQLSTATE 23505) — caught internally and reported as the now-idempotent
 * existing state instead of a raw 500.
 *
 * KNOWN GAP (for the webhook phase — nothing exists yet to reverse
 * anything): a refund that the provider later reverses/voids has no
 * representation in this schema. The `billing_refunds` row from the
 * original refund event would stay in place, permanently counting toward
 * the aggregate total in step 3 — a legitimate LATER refund on the same
 * payment could then be wrongly refused as "cumulative total exceeds
 * amount_cents" even though the voided refund never actually took money out
 * a second time. The webhook handler should account for this (e.g. a
 * refund-reversed event needs its own handling here, not just a bigger
 * refund) before it's reachable in production.
 *
 * @throws Repositories::PaymentNotFound if @p provider_capture_id is unknown.
 * @throws InvalidRefundAmount if @p refunded_amount_cents is out of range,
 *         single-call or cumulative.
 */
CreditResult refund_capture(const std::string& provider_capture_id,
                            const std::string& provider_refund_id,
                            std::int64_t refunded_amount_cents);

/**
 * @brief Admin manual entry (positive or negative). The caller is
 *        responsible for audit-logging this action (mirrors how every other
 *        admin mutation in this codebase writes its own audit_log row) —
 *        this function only owns the ledger + balance write.
 *
 * @p user_id and @p admin_id are both foreign keys on the SAME
 * wallet_entries insert (`user_id`, `created_by`); a bare SQLSTATE from that
 * insert can't say which one was bad, so both are validated explicitly,
 * user_id first, as their own standalone reads BEFORE the write transaction
 * opens (see detail::check_user_id in Wallet.cpp — the same "don't catch
 * 22P02 mid-transaction" reasoning that governs refund_capture's design).
 * The balance upsert locks wallet_balances first and computes the new total
 * in C++ — see the file-level note on why it never uses a self-referencing
 * SQL expression.
 *
 * @throws ZeroAdjustment if @p delta_credits is 0.
 * @throws MalformedUserId / UnknownUser if @p user_id is malformed / unknown.
 * @throws MalformedAdminId / UnknownAdmin if @p admin_id is malformed / unknown.
 * @throws InsufficientBalance if a negative @p delta_credits would drive the
 *         balance below zero. Unlike refund_capture, no real-world money has
 *         moved yet for an adjustment, so it is refused outright rather than
 *         applied with a reconciliation warning.
 */
CreditResult adjust(const std::string& user_id,
                    std::int64_t delta_credits,
                    const std::string& note,
                    const std::string& admin_id);

/**
 * @brief Idempotent debit — the `kind='spend'` writer that migration 007
 *        declared but nothing implemented (the documented gap from the
 *        downstream port, #24/#39). Writes a negative `spend` ledger row and
 *        the balance-cache update in ONE transaction, or refuses cleanly.
 *
 * Deliberately NOT exposed as an HTTP endpoint by this template: WHAT a
 * credit buys — a render, an API call, a report — is fork domain. A fork's
 * service layer calls this function at its charge point (with the fork's own
 * natural idempotency key as @p reference: a job id, an order id, a request
 * id); the template only guarantees that whatever that charge is, it can
 * never post twice, never overdraw, and never desync the ledger from the
 * cache. docs/openapi.yaml and Api::get_endpoints() are intentionally
 * untouched.
 *
 * Same canonical write pattern as every other writer in this file:
 * materialize the wallet_balances row (`INSERT ... ON CONFLICT DO NOTHING`),
 * `SELECT ... FOR UPDATE`, compute the new total in C++ from the locked
 * read, plain `UPDATE` — see the file-level note on why never a
 * self-referencing SQL expression. Lock order: spend takes ONLY the
 * wallet_balances lock (there is no payments row in this flow) — that is the
 * SECOND lock in credit_capture's and refund_capture's order, with no first
 * lock held before it, so spend can never deadlock against either of them
 * over the same user.
 *
 * Decision sequence inside the transaction, all under the balance-row lock:
 *
 *   1. Idempotency: if a `kind='spend'` wallet_entries row already carries
 *      @p reference, this is a retry — return the existing state
 *      (`credited=false`), write nothing. Checked UNDER the lock, so a
 *      same-user concurrent duplicate serializes on the balance row and the
 *      loser sees the winner's committed row here instead of racing it. If
 *      the existing row belongs to a DIFFERENT user, throw
 *      DuplicateSpendReference instead — a retry can legitimately repeat a
 *      reference, but never across wallets.
 *   2. Sufficiency, in C++: `current < credits` throws InsufficientCredits
 *      and rolls the transaction back — NO ledger row, no balance change, no
 *      durable record of the refusal (unlike refund_capture's skip marker:
 *      a refused spend moved no real-world money anywhere, so there is
 *      nothing to reconcile — the caller just gets its 409 and decides). The
 *      wallet_balances CHECK (credits >= 0) stays as defense in depth only.
 *   3. Otherwise: `INSERT INTO wallet_entries (..., -credits, 'spend',
 *      reference, note)` + the plain balance UPDATE, same transaction.
 *
 * A concurrent duplicate that slips past step 1 — two racing calls whose
 * DIFFERENT user ids share a reference hold different balance-row locks, so
 * nothing serializes them — loses on migration 009's partial UNIQUE index
 * (SQLSTATE 23505), caught and reported as the idempotent existing state
 * (same user) or DuplicateSpendReference (different user), never a raw 500.
 * Same-user duplicates never reach the index: they serialize on the balance
 * lock and the loser resolves at step 1.
 *
 * @param user_id   wallet owner (validated up front like adjust's ids —
 *                  detail::check_user_id, outside the write transaction).
 * @param credits   amount to debit, must be > 0.
 * @param reference REQUIRED non-empty idempotency key, unique across all
 *                  spend rows (migration 009).
 * @param note      free-form context stored on the ledger row ('' is fine).
 *
 * @throws InvalidSpendAmount if @p credits <= 0.
 * @throws MissingSpendReference if @p reference is empty.
 * @throws MalformedUserId / UnknownUser if @p user_id is malformed / unknown.
 * @throws InsufficientCredits if the locked balance can't cover @p credits.
 * @throws DuplicateSpendReference if @p reference already names a different
 *         user's spend.
 */
CreditResult spend(const std::string& user_id,
                   std::int64_t credits,
                   const std::string& reference,
                   const std::string& note = "");

}  // namespace Billing
