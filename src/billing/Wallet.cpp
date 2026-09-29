/**
 * @file Wallet.cpp
 * @brief Bodies for src/billing/Wallet.hpp — compiled once into app_core.
 *        Every money invariant and its rationale is documented on the
 *        declarations in the header; the comments here explain the SQL
 *        mechanics in place.
 */

#include "billing/Wallet.hpp"

#include <pqxx/pqxx>
#include <string_view>

#include <spdlog/spdlog.h>

#include "database/Database.hpp"
#include "repositories/BillingRepository.hpp"  // Repositories::PaymentNotFound
#include "repositories/SqlErrors.hpp"          // Repositories::detail::translate_sql

namespace Billing {

namespace detail {

/// Current cached balance for @p user_id, read through the caller's own
/// transaction (never opens a second connection) — 0 if the user has never
/// had a wallet_balances row written.
template <typename Txn>
std::int64_t read_balance(Txn& txn, const std::string& user_id) {
    auto r = txn.exec_params("SELECT credits FROM wallet_balances WHERE user_id = $1", user_id);
    if (r.empty())
        return 0;
    return r[0]["credits"].template as<std::int64_t>();
}

/// Result of validating a single id against `users`, without ever letting a
/// malformed literal's SQLSTATE 22P02 escape mid-transaction (see
/// check_user_id's doc for why that matters).
enum class IdCheck { Valid, Malformed, Unknown };

/**
 * @brief Is @p id a well-formed UUID that references an existing user?
 *        Run as its OWN standalone read — never inside the transaction that
 *        will later write using this id. A malformed literal raises 22P02;
 *        catching that INSIDE a `pqxx::work` would leave the transaction
 *        aborted for every statement after it (Postgres: one failed
 *        statement poisons the rest of the transaction until rollback), so
 *        this check must fully complete and close its own transaction
 *        before the caller opens the real write. Reads the PRIMARY: a user
 *        or admin id created moments earlier (e.g. a signup immediately
 *        followed by a welcome-bonus adjust(), or a role just promoted to
 *        admin) must not be misreported as unknown because of replica lag.
 */
IdCheck check_user_id(const std::string& id) {
    try {
        return Database::get().execute_read_primary([&](auto& txn) {
            auto r = txn.exec_params("SELECT 1 FROM users WHERE id = $1", id);
            return r.empty() ? IdCheck::Unknown : IdCheck::Valid;
        });
    } catch (const pqxx::sql_error& e) {
        if (std::string_view(e.sqlstate()) == "22P02")
            return IdCheck::Malformed;
        throw;
    }
}

}  // namespace detail

std::int64_t balance_of(const std::string& user_id, bool from_primary) {
    auto query = [&](auto& txn) -> std::int64_t { return detail::read_balance(txn, user_id); };
    return from_primary ? Database::get().execute_read_primary(query) : Database::get().execute_read(query);
}

std::vector<Domain::WalletEntry> history(const std::string& user_id, int limit, int offset, bool from_primary) {
    auto query = [&](auto& txn) {
        auto r = txn.exec_params(
            "SELECT id, user_id, delta_credits, kind, reference, note, created_by, created_at "
            "FROM wallet_entries WHERE user_id = $1 ORDER BY created_at DESC LIMIT $2 OFFSET $3",
            user_id,
            limit,
            offset);
        std::vector<Domain::WalletEntry> out;
        out.reserve(r.size());
        for (const auto& row : r)
            out.push_back(Domain::WalletEntry::from_row(row));
        return out;
    };
    return from_primary ? Database::get().execute_read_primary(query) : Database::get().execute_read(query);
}

CreditResult credit_capture(const std::string& provider_order_id,
                            const std::string& provider_capture_id,
                            std::int64_t captured_amount_cents,
                            const std::string& captured_currency) {
    return Repositories::detail::translate_sql(
        [&] {
            return Database::get().execute_write([&](auto& txn) -> CreditResult {
                // Idempotency guard: only a payment that has never been captured
                // matches this UPDATE. A same-order concurrent duplicate
                // (return-flow racing the webhook) blocks on this row's lock and
                // then loses the WHERE once the winner commits, landing in the
                // empty-result branch below. If provider_capture_id already
                // belongs to a DIFFERENT order (pathological — provider capture
                // ids are unique by construction) the UNIQUE index raises 23505,
                // translated to DuplicateCaptureId below instead of surfacing as
                // a bare 500.
                auto ur = txn.exec_params(
                    "UPDATE payments SET provider_capture_id = $1, status = 'captured' "
                    "WHERE provider_order_id = $2 AND provider_capture_id IS NULL "
                    "RETURNING id, user_id, credits_expected, amount_cents, currency",
                    provider_capture_id,
                    provider_order_id);

                if (ur.empty()) {
                    // Already captured, or an unknown order — re-read and report
                    // the existing state instead of crediting a second time.
                    auto pr = txn.exec_params("SELECT id, user_id FROM payments WHERE provider_order_id = $1",
                                              provider_order_id);
                    if (pr.empty())
                        throw Repositories::PaymentNotFound{};
                    const std::string payment_id = pr[0]["id"].template as<std::string>();
                    const std::string user_id = pr[0]["user_id"].template as<std::string>();
                    return CreditResult{false, detail::read_balance(txn, user_id), payment_id};
                }

                const std::string payment_id = ur[0]["id"].template as<std::string>();
                const std::string user_id = ur[0]["user_id"].template as<std::string>();
                const std::int64_t credits_expected = ur[0]["credits_expected"].template as<std::int64_t>();
                const std::int64_t amount_cents = ur[0]["amount_cents"].template as<std::int64_t>();
                const std::string currency = ur[0]["currency"].template as<std::string>();

                const bool amount_ok = captured_amount_cents == amount_cents;
                const bool currency_ok = captured_currency == currency;
                if (!amount_ok || !currency_ok) {
                    // The capture-id UPDATE above already ran, so re-processing
                    // this exact order can never reach this branch again
                    // (provider_capture_id is no longer NULL) — the failure is
                    // terminal, not retried.
                    std::string reason;
                    if (!amount_ok)
                        reason += "amount mismatch: captured=" + std::to_string(captured_amount_cents) +
                                  " expected=" + std::to_string(amount_cents);
                    if (!currency_ok) {
                        if (!reason.empty())
                            reason += "; ";
                        reason += "currency mismatch: captured=" + captured_currency + " expected=" + currency;
                    }
                    txn.exec_params(
                        "UPDATE payments SET status = 'failed', failure_reason = $1 WHERE id = $2", reason, payment_id);
                    spdlog::error(
                        "billing: capture mismatch for order {} (payment {}): {} — payment marked failed, wallet "
                        "untouched",
                        provider_order_id,
                        payment_id,
                        reason);
                    return CreditResult{false, detail::read_balance(txn, user_id), payment_id};
                }

                txn.exec_params(
                    "INSERT INTO wallet_entries (user_id, delta_credits, kind, reference) VALUES ($1, $2, 'topup', $3)",
                    user_id,
                    credits_expected,
                    payment_id);

                // Materialize the row FIRST so the lock below always has
                // something to hold. `SELECT ... FOR UPDATE` cannot lock a
                // row that doesn't exist yet: for a brand-new user (no
                // wallet_balances row), the FIRST pair of concurrent
                // balance-changing writes would both fall through to
                // `current = 0` unlocked, and the second writer's upsert
                // would silently overwrite the first's result with a
                // stale-based total — wallet_balances.credits then disagrees
                // with SUM(wallet_entries.delta_credits) even though the
                // ledger itself stays correct. `DO NOTHING` is safe: this
                // statement only needs a row to exist so the next statement
                // has something to lock; the real value is written by the
                // plain UPDATE below, under that lock.
                txn.exec_params(
                    "INSERT INTO wallet_balances (user_id, credits) VALUES ($1, 0) ON CONFLICT (user_id) DO NOTHING",
                    user_id);

                // Lock the balance row (now guaranteed to exist) and compute
                // the new total explicitly — see the file-level note on why
                // this never uses `SET credits = wallet_balances.credits +
                // EXCLUDED.credits`.
                auto wb = txn.exec_params("SELECT credits FROM wallet_balances WHERE user_id = $1 FOR UPDATE", user_id);
                const std::int64_t current_balance = wb[0]["credits"].template as<std::int64_t>();
                const std::int64_t new_total = current_balance + credits_expected;
                // Plain UPDATE, not an upsert: the row is provably present
                // (materialized above) and locked (SELECT ... FOR UPDATE
                // above), so there is nothing left to conflict against.
                auto br = txn.exec_params(
                    "UPDATE wallet_balances SET credits = $2, updated_at = now() WHERE user_id = $1 RETURNING credits",
                    user_id,
                    new_total);

                return CreditResult{true, br[0]["credits"].template as<std::int64_t>(), payment_id};
            });
        },
        [](std::string_view ss) {
            if (ss == "23505")
                throw DuplicateCaptureId{};
        });
}

CreditResult refund_capture(const std::string& provider_capture_id,
                            const std::string& provider_refund_id,
                            std::int64_t refunded_amount_cents) {
    auto attempt = [&](auto& txn) -> CreditResult {
        // FOR UPDATE: see step 1 in the header docs — this is what makes the
        // aggregate check below race-safe against a concurrent refund on the
        // same payment, and fixes this function's lock order to match
        // credit_capture's (payments, then wallet_balances).
        auto pr = txn.exec_params(
            "SELECT id, user_id, amount_cents, credits_expected FROM payments WHERE provider_capture_id = $1 "
            "FOR UPDATE",
            provider_capture_id);
        if (pr.empty())
            throw Repositories::PaymentNotFound{};
        const std::string payment_id = pr[0]["id"].template as<std::string>();
        const std::string user_id = pr[0]["user_id"].template as<std::string>();
        const std::int64_t amount_cents = pr[0]["amount_cents"].template as<std::int64_t>();
        const std::int64_t credits_expected = pr[0]["credits_expected"].template as<std::int64_t>();

        // Idempotency FIRST, via the durable marker — see the function docs
        // for why this can no longer be "does a wallet_entries row exist".
        auto existing =
            txn.exec_params("SELECT id FROM billing_refunds WHERE provider_refund_id = $1", provider_refund_id);
        if (!existing.empty()) {
            return CreditResult{false, detail::read_balance(txn, user_id), payment_id};
        }

        if (refunded_amount_cents <= 0 || refunded_amount_cents > amount_cents) {
            spdlog::error(
                "billing: refund {} for payment {} (capture {}) has an out-of-range amount: refunded={} "
                "payment_amount={} — refused, nothing written",
                provider_refund_id,
                payment_id,
                provider_capture_id,
                refunded_amount_cents,
                amount_cents);
            throw InvalidRefundAmount{};
        }

        // Trustworthy under concurrency because the payments row is already
        // locked (step 1) — a second concurrent refund on this same payment
        // blocked there until this transaction commits or rolls back.
        auto sum_row = txn.exec_params(
            "SELECT COALESCE(SUM(amount_cents), 0) AS total FROM billing_refunds WHERE payment_id = $1", payment_id);
        const std::int64_t already_refunded = sum_row[0]["total"].template as<std::int64_t>();
        const std::int64_t cumulative_total = already_refunded + refunded_amount_cents;
        if (cumulative_total > amount_cents) {
            spdlog::error(
                "billing: refund {} for payment {} would push cumulative refunds past the payment amount: "
                "already_refunded={} this_refund={} payment_amount={} — refused, nothing written",
                provider_refund_id,
                payment_id,
                already_refunded,
                refunded_amount_cents,
                amount_cents);
            throw InvalidRefundAmount{};
        }

        // Prorate off credits_expected, NOT rate_snapshot — see the
        // function-level doc comment in the header. `(credits_expected *
        // amount_cents) / amount_cents == credits_expected` exactly for a
        // full refund, no matter how credits_expected was priced (package or
        // custom).
        const std::int64_t refund_credits = (credits_expected * refunded_amount_cents) / amount_cents;

        // Materialize the row FIRST so the lock below always has something
        // to hold — see credit_capture's comments for the full lost-update
        // diagnosis this guards against (a not-yet-existing row can't be
        // locked by `FOR UPDATE`, so the first pair of concurrent
        // balance-changing writes for a brand-new user could both read
        // current=0 unlocked and the second silently clobber the first).
        // `DO NOTHING` is safe: only existence matters here, not the value.
        txn.exec_params(
            "INSERT INTO wallet_balances (user_id, credits) VALUES ($1, 0) ON CONFLICT (user_id) DO NOTHING", user_id);

        // Lock the balance row (now guaranteed to exist) ONCE, up front,
        // whatever the outcome turns out to be — its value seeds the
        // response in all three branches below and is the
        // sufficiency-decision input in one of them, so there's no second
        // (and, on the zero-credit path, wasted) query.
        auto br = txn.exec_params("SELECT credits FROM wallet_balances WHERE user_id = $1 FOR UPDATE", user_id);
        std::int64_t new_balance = br[0]["credits"].template as<std::int64_t>();

        std::string outcome;
        std::int64_t credits_deducted = 0;

        if (refund_credits == 0) {
            // Too small to represent at this payment's credits_expected/
            // amount_cents ratio — inserting delta_credits=0 would trip
            // wallet_entries' CHECK (delta_credits <> 0). Record the attempt
            // and move on instead of letting that escape as a raw constraint
            // violation (that used to 500 and get redelivered by the
            // provider forever).
            outcome = "skipped_zero_credits";
            spdlog::error(
                "billing: refund {} for payment {} converts to 0 credits (credits_expected={} amount_cents={} "
                "refunded_amount_cents={}) — recorded, wallet untouched; manual reconciliation may be needed",
                provider_refund_id,
                payment_id,
                credits_expected,
                amount_cents,
                refunded_amount_cents);
        } else if (new_balance - refund_credits < 0) {
            outcome = "skipped_insufficient";
            spdlog::error(
                "billing: refund {} for payment {} user {} needs {} credits but only {} are available — "
                "recorded, wallet NOT debited; manual reconciliation required",
                provider_refund_id,
                payment_id,
                user_id,
                refund_credits,
                new_balance);
        } else {
            // Compute the new total explicitly from the locked read above and
            // set it directly — never via `SET credits = wallet_balances.credits
            // + EXCLUDED.credits` (see the header's file-level note).
            const std::int64_t computed_new_balance = new_balance - refund_credits;
            txn.exec_params(
                "INSERT INTO wallet_entries (user_id, delta_credits, kind, reference) VALUES ($1, $2, 'refund', $3)",
                user_id,
                -refund_credits,
                provider_refund_id);
            // Plain UPDATE, not an upsert: the row is provably present
            // (materialized above) and locked (SELECT ... FOR UPDATE above).
            auto updated = txn.exec_params(
                "UPDATE wallet_balances SET credits = $2, updated_at = now() WHERE user_id = $1 RETURNING credits",
                user_id,
                computed_new_balance);
            new_balance = updated[0]["credits"].template as<std::int64_t>();
            outcome = "applied";
            credits_deducted = refund_credits;
        }

        // The durable marker — written for every outcome, so a redelivery of
        // this exact refund id always short-circuits at the idempotency
        // check above, whatever happened the first time.
        txn.exec_params(
            "INSERT INTO billing_refunds (payment_id, provider_refund_id, amount_cents, credits_deducted, outcome) "
            "VALUES ($1, $2, $3, $4, $5)",
            payment_id,
            provider_refund_id,
            refunded_amount_cents,
            credits_deducted,
            outcome);

        // The CUMULATIVE total (not just this call's amount) reaching the
        // full payment amount flips a currently-captured payment to
        // refunded — regardless of whether the wallet could be debited (the
        // money left the provider either way). A cumulative total still
        // short of the full amount, or a payment that isn't 'captured'
        // (e.g. 'failed'), leaves status untouched (0 rows affected below is
        // not an error).
        txn.exec_params(
            "UPDATE payments SET status = 'refunded' WHERE id = $1 AND status = 'captured' AND $2 >= amount_cents",
            payment_id,
            cumulative_total);

        // credited=true past this point unconditionally: this call durably
        // recorded a NEW billing_refunds row either way — see the header's
        // note on CreditResult::credited. Only the idempotency short-circuit
        // above (an already-seen refund id) returns credited=false.
        return CreditResult{true, new_balance, payment_id};
    };

    try {
        return Database::get().execute_write(attempt);
    } catch (const pqxx::sql_error& e) {
        if (std::string_view(e.sqlstate()) != "23505")
            throw;
        // Lost a race against a concurrent identical refund id — the winner's
        // insert already applied. Report the current state instead of
        // retrying; do NOT re-attempt the write ourselves. Read the PRIMARY:
        // the winner just committed there, and a replica could still be lagging.
        auto pr = Database::get().execute_read_primary([&](auto& txn) {
            return txn.exec_params("SELECT id, user_id FROM payments WHERE provider_capture_id = $1",
                                   provider_capture_id);
        });
        if (pr.empty())
            throw Repositories::PaymentNotFound{};
        const std::string payment_id = pr[0]["id"].template as<std::string>();
        const std::string user_id = pr[0]["user_id"].template as<std::string>();
        return CreditResult{false, balance_of(user_id, /*from_primary=*/true), payment_id};
    }
}

CreditResult adjust(const std::string& user_id,
                    std::int64_t delta_credits,
                    const std::string& note,
                    const std::string& admin_id) {
    if (delta_credits == 0)
        throw ZeroAdjustment{};

    switch (detail::check_user_id(user_id)) {
        case detail::IdCheck::Malformed:
            throw MalformedUserId{};
        case detail::IdCheck::Unknown:
            throw UnknownUser{};
        case detail::IdCheck::Valid:
            break;
    }
    switch (detail::check_user_id(admin_id)) {
        case detail::IdCheck::Malformed:
            throw MalformedAdminId{};
        case detail::IdCheck::Unknown:
            throw UnknownAdmin{};
        case detail::IdCheck::Valid:
            break;
    }

    return Repositories::detail::translate_sql(
        [&] {
            return Database::get().execute_write([&](auto& txn) -> CreditResult {
                // Materialize the row FIRST so the lock below always has
                // something to hold — see credit_capture's comments for
                // the full lost-update diagnosis this guards against (a
                // not-yet-existing row can't be locked by `FOR UPDATE`, so
                // the first pair of concurrent balance-changing writes for a
                // brand-new user could both read current=0 unlocked and the
                // second silently clobber the first). `DO NOTHING` is safe:
                // only existence matters here, not the value.
                txn.exec_params(
                    "INSERT INTO wallet_balances (user_id, credits) VALUES ($1, 0) ON CONFLICT (user_id) DO NOTHING",
                    user_id);

                // Lock the balance row (now guaranteed to exist) and compute
                // the new total explicitly — see the header's file-level note
                // on why this never uses `SET credits = wallet_balances.credits
                // + EXCLUDED.credits`. A negative new_total is still written
                // as-is: the CHECK (credits >= 0) then reliably rejects it
                // (this is now a plain "does this literal value satisfy the
                // constraint" check, not a self-referencing expression),
                // caught below as SQLSTATE 23514.
                auto wb = txn.exec_params("SELECT credits FROM wallet_balances WHERE user_id = $1 FOR UPDATE", user_id);
                const std::int64_t current_balance = wb[0]["credits"].template as<std::int64_t>();
                const std::int64_t new_total = current_balance + delta_credits;

                txn.exec_params(
                    "INSERT INTO wallet_entries (user_id, delta_credits, kind, reference, note, created_by) "
                    "VALUES ($1, $2, 'adjustment', '', $3, $4)",
                    user_id,
                    delta_credits,
                    note,
                    admin_id);
                // Plain UPDATE, not an upsert: the row is provably present
                // (materialized above) and locked (SELECT ... FOR UPDATE
                // above), so there is nothing left to conflict against.
                auto br = txn.exec_params(
                    "UPDATE wallet_balances SET credits = $2, updated_at = now() WHERE user_id = $1 RETURNING credits",
                    user_id,
                    new_total);
                return CreditResult{true, br[0]["credits"].template as<std::int64_t>(), std::string{}};
            });
        },
        [](std::string_view ss) {
            if (ss == "23514")
                throw InsufficientBalance{};
            // Defense in depth only — the pre-checks above make these two
            // unreachable except for an extremely narrow TOCTOU race (a user
            // deleted between the check and this write). Attribute to
            // user_id, the more common cause, rather than silently 500ing.
            if (ss == "23503")
                throw UnknownUser{};
            if (ss == "22P02")
                throw MalformedUserId{};
        });
}

CreditResult spend(const std::string& user_id,
                   std::int64_t credits,
                   const std::string& reference,
                   const std::string& note) {
    if (credits <= 0)
        throw InvalidSpendAmount{};
    if (reference.empty())
        throw MissingSpendReference{};

    switch (detail::check_user_id(user_id)) {
        case detail::IdCheck::Malformed:
            throw MalformedUserId{};
        case detail::IdCheck::Unknown:
            throw UnknownUser{};
        case detail::IdCheck::Valid:
            break;
    }

    auto attempt = [&](auto& txn) -> CreditResult {
        // Materialize the row FIRST so the lock below always has something
        // to hold — see credit_capture's comments for the full lost-update
        // diagnosis this guards against. `DO NOTHING` is safe: only
        // existence matters here, not the value.
        txn.exec_params(
            "INSERT INTO wallet_balances (user_id, credits) VALUES ($1, 0) ON CONFLICT (user_id) DO NOTHING", user_id);

        // Lock the balance row (now guaranteed to exist) BEFORE the
        // idempotency check: a same-user concurrent duplicate then
        // serializes here, and the loser's check below reliably sees the
        // winner's committed spend row instead of racing it (mirrors
        // refund_capture, where the payments lock plays this role).
        auto wb = txn.exec_params("SELECT credits FROM wallet_balances WHERE user_id = $1 FOR UPDATE", user_id);
        const std::int64_t current_balance = wb[0]["credits"].template as<std::int64_t>();

        // Idempotency: the reference IS the marker — unlike a skipped
        // refund, a refused spend writes nothing at all (see below), so a
        // ledger-row check is sufficient here where it wasn't for refunds.
        auto existing =
            txn.exec_params("SELECT user_id FROM wallet_entries WHERE kind = 'spend' AND reference = $1", reference);
        if (!existing.empty()) {
            if (existing[0]["user_id"].template as<std::string>() != user_id)
                throw DuplicateSpendReference{};
            return CreditResult{false, current_balance, std::string{}};
        }

        // Sufficiency in C++, under the lock, BEFORE any write — the
        // refusal must be a clean 409 with an untouched ledger, never the
        // wallet_balances CHECK (credits >= 0) firing as a raw 23514. The
        // whole transaction (including the materialize insert above) rolls
        // back; deliberately NO durable record of the refusal — no
        // real-world money moved anywhere, so there is nothing to
        // reconcile (contrast refund_capture's skipped_* markers).
        if (current_balance < credits) {
            spdlog::info("billing: spend '{}' for user {} needs {} credits but only {} are available — refused",
                         reference,
                         user_id,
                         credits,
                         current_balance);
            throw InsufficientCredits{};
        }

        // Compute the new total explicitly from the locked read above and
        // set it directly — never via a self-referencing SQL expression
        // (see the header's file-level note).
        const std::int64_t new_total = current_balance - credits;
        txn.exec_params(
            "INSERT INTO wallet_entries (user_id, delta_credits, kind, reference, note) "
            "VALUES ($1, $2, 'spend', $3, $4)",
            user_id,
            -credits,
            reference,
            note);
        // Plain UPDATE, not an upsert: the row is provably present
        // (materialized above) and locked (SELECT ... FOR UPDATE above).
        auto br = txn.exec_params(
            "UPDATE wallet_balances SET credits = $2, updated_at = now() WHERE user_id = $1 RETURNING credits",
            user_id,
            new_total);
        return CreditResult{true, br[0]["credits"].template as<std::int64_t>(), std::string{}};
    };

    try {
        return Database::get().execute_write(attempt);
    } catch (const pqxx::sql_error& e) {
        const std::string_view ss = e.sqlstate();
        // Defense in depth — the C++ sufficiency check under the lock makes
        // the CHECK (credits >= 0) unreachable, and the pre-check makes
        // 23503 a narrow TOCTOU (user deleted between check and write).
        if (ss == "23514")
            throw InsufficientCredits{};
        if (ss == "23503")
            throw UnknownUser{};
        if (ss != "23505")
            throw;
        // Lost a race on migration 009's partial UNIQUE index — only
        // reachable for a cross-user reference collision (same-user
        // duplicates serialize on the balance lock and resolve at the
        // idempotency check). Re-read the winner's row from the PRIMARY (it
        // just committed there; a replica could still be lagging) and report
        // instead of retrying.
        auto winner = Database::get().execute_read_primary([&](auto& txn) {
            return txn.exec_params("SELECT user_id FROM wallet_entries WHERE kind = 'spend' AND reference = $1",
                                   reference);
        });
        if (winner.empty() || winner[0]["user_id"].template as<std::string>() != user_id)
            throw DuplicateSpendReference{};
        return CreditResult{false, balance_of(user_id, /*from_primary=*/true), std::string{}};
    }
}

}  // namespace Billing
