/**
 * @file test_wallet.cpp
 * @brief Integration tests for the billing module's money core:
 *        src/billing/Wallet.hpp (Billing::credit_capture / refund_capture /
 *        adjust / spend / balance_of / history) plus src/repositories/BillingRepository.hpp
 *        (Repositories::PackageRepository / PaymentRepository /
 *        BillingSettingsRepository).
 *
 * Needs the billing migrations (007/008/009) — applied unconditionally regardless
 * of the billing.enabled flag, same pattern as test_post_repository.cpp for
 * the content module. Also covers UserRepository::remove() mapping the ON
 * DELETE RESTRICT foreign keys (payments, wallet_entries, wallet_balances →
 * users) to a typed 409 instead of a bare pqxx::sql_error / 500.
 */

#include <atomic>
#include <random>
#include <thread>

#include <gtest/gtest.h>

#include "billing/PayPalClient.hpp"
#include "billing/Wallet.hpp"
#include "domain/Billing.hpp"
#include "repositories/BillingRepository.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;

namespace {

class WalletTest : public TestHelpers::CoreBackedTest {
protected:
    Repositories::RoleRepository roles;
    Repositories::UserRepository users;
    Repositories::PaymentRepository payments;
    Repositories::PackageRepository packages;
    Repositories::BillingSettingsRepository settings;

    std::string config_file_name() const override { return "wallet_test_config.json"; }

    void config_overrides(json& cfg) override {
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
        cfg["billing"]["enabled"] = true;
        // Core::initialize() calls Billing::initialize(), which THROWS when
        // billing.enabled=true and client_id/client_secret/webhook_id are
        // empty (the fail-at-boot guard). Provide obviously-fake sandbox
        // values — nothing in this suite ever talks to the network.
        cfg["billing"]["paypal"]["environment"] = "sandbox";
        cfg["billing"]["paypal"]["client_id"] = "test-client-id";
        cfg["billing"]["paypal"]["client_secret"] = "test-client-secret";
        cfg["billing"]["paypal"]["webhook_id"] = "test-webhook-id";
    }

    void SetUp() override {
        // A previous suite's Core boot may have installed a PayPal client
        // built from ITS config (Billing::initialize is a keep-first no-op) —
        // drop it so this suite's boot installs one from this suite's config.
        Billing::reset_for_testing();
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Database::get().execute_write([](auto& txn) {
            txn.exec(
                "TRUNCATE TABLE wallet_entries, wallet_balances, billing_refunds, payments, billing_packages CASCADE");
            txn.exec("TRUNCATE TABLE users CASCADE");
            txn.exec("DELETE FROM roles WHERE name NOT IN ('User', 'Administrator')");
            // billing_settings is seeded exactly once by migration 008 and is
            // never truncated (its repository assumes the single row always
            // exists) — reset it to the known defaults so a settings-mutating
            // test can't leak state into whichever test runs next.
            txn.exec(
                "UPDATE billing_settings SET credits_per_unit = 100, min_amount_cents = 100, "
                "max_amount_cents = 100000 WHERE id = 1");
            return 0;
        });
    }

    // billing_settings is a single, never-truncated row shared with every
    // other billing suite in this binary — reset it on the way out too, not
    // just on the way in, so a settings-mutating test here can't leave a
    // non-default rate for whichever suite the test runner happens to
    // execute next.
    void TearDown() override {
        Billing::reset_for_testing();
        if (!::testing::Test::IsSkipped()) {
            Database::get().execute_write([](auto& txn) {
                txn.exec(
                    "UPDATE billing_settings SET credits_per_unit = 100, min_amount_cents = 100, "
                    "max_amount_cents = 100000 WHERE id = 1");
                return 0;
            });
        }
        TestHelpers::CoreBackedTest::TearDown();
    }

    std::string seed_user(const std::string& email) {
        auto role = roles.find_by_name("User");
        if (!role) {
            ADD_FAILURE() << "role User missing — seed migration?";
            throw std::runtime_error("seed role missing: User");
        }
        auto created = users.create(
            email, std::string("$argon2id$placeholder"), std::nullopt, std::nullopt, role->id, /*confirmed=*/true);
        return created.id;
    }

    // Default rate: 100 credits per 100 cents (1 credit per cent) — keeps the
    // arithmetic in every test trivially readable. Currency is always "USD"
    // here; the currency-mismatch test overrides it at the credit_capture call.
    Domain::Payment seed_payment(const std::string& user_id,
                                 const std::string& order_id,
                                 std::int64_t amount_cents = 1000,
                                 std::int64_t credits_expected = 1000,
                                 std::int64_t rate_snapshot = 100) {
        return payments.create(user_id, order_id, amount_cents, "USD", credits_expected, rate_snapshot, std::nullopt);
    }

    // billing_refunds has no repository (it's Wallet.hpp's own internal
    // idempotency marker) — read it directly for assertions.
    std::optional<std::string> refund_outcome(const std::string& provider_refund_id) {
        return Database::get().execute_read([&](auto& txn) -> std::optional<std::string> {
            auto r = txn.exec_params("SELECT outcome FROM billing_refunds WHERE provider_refund_id = $1",
                                     provider_refund_id);
            if (r.empty())
                return std::nullopt;
            return r[0]["outcome"].template as<std::string>();
        });
    }

    long refund_row_count(const std::string& payment_id) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params("SELECT COUNT(*) FROM billing_refunds WHERE payment_id = $1", payment_id);
            return r[0][0].template as<long>();
        });
    }
};

// ── credit_capture ──────────────────────────────────────────────────────────

TEST_F(WalletTest, CreditCaptureCreditsOnceAndUpdatesBalance) {
    auto user_id = seed_user("buyer1@example.com");
    seed_payment(user_id, "ORDER-1", /*amount_cents=*/1000, /*credits_expected=*/1000);

    auto result = Billing::credit_capture("ORDER-1", "CAPTURE-1", /*captured_amount_cents=*/1000, "USD");
    EXPECT_TRUE(result.credited);
    EXPECT_EQ(result.balance, 1000);
    EXPECT_FALSE(result.payment_id.empty());

    EXPECT_EQ(Billing::balance_of(user_id), 1000);

    auto hist = Billing::history(user_id, 10, 0);
    ASSERT_EQ(hist.size(), 1u);
    EXPECT_EQ(hist[0].delta_credits, 1000);
    EXPECT_EQ(hist[0].kind, "topup");
    EXPECT_EQ(hist[0].reference, result.payment_id);

    auto found = payments.find(result.payment_id);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "captured");
    ASSERT_TRUE(found->provider_capture_id.has_value());
    EXPECT_EQ(*found->provider_capture_id, "CAPTURE-1");

    // from_primary=true must agree with the default (replica-tolerant) read.
    EXPECT_EQ(Billing::balance_of(user_id, /*from_primary=*/true), 1000);
    EXPECT_EQ(Billing::history(user_id, 10, 0, /*from_primary=*/true).size(), 1u);
}

TEST_F(WalletTest, CreditCaptureIsIdempotentOnCaptureId) {
    auto user_id = seed_user("buyer2@example.com");
    seed_payment(user_id, "ORDER-2", 500, 500);

    auto first = Billing::credit_capture("ORDER-2", "CAPTURE-2", 500, "USD");
    ASSERT_TRUE(first.credited);
    ASSERT_EQ(first.balance, 500);

    auto second = Billing::credit_capture("ORDER-2", "CAPTURE-2", 500, "USD");
    EXPECT_FALSE(second.credited);
    EXPECT_EQ(second.balance, 500);
    EXPECT_EQ(second.payment_id, first.payment_id);

    // Exactly one ledger row — the second call touched nothing.
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 1u);
    EXPECT_EQ(Billing::balance_of(user_id), 500);
}

TEST_F(WalletTest, CreditCaptureRefusesAmountMismatch) {
    auto user_id = seed_user("buyer3@example.com");
    auto payment = seed_payment(user_id, "ORDER-3", 1000, 1000);

    auto result = Billing::credit_capture("ORDER-3", "CAPTURE-3", /*captured_amount_cents=*/999, "USD");
    EXPECT_FALSE(result.credited);
    EXPECT_EQ(result.balance, 0);

    // No ledger row at all.
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 0u);
    EXPECT_EQ(Billing::balance_of(user_id), 0);

    auto found = payments.find(payment.id);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "failed");
    ASSERT_TRUE(found->failure_reason.has_value());
    EXPECT_NE(found->failure_reason->find("mismatch"), std::string::npos);

    // The capture id was still recorded, so a retried mismatch never reopens
    // this order for crediting.
    auto retry = Billing::credit_capture("ORDER-3", "CAPTURE-3", 999, "USD");
    EXPECT_FALSE(retry.credited);
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 0u);
}

TEST_F(WalletTest, CreditCaptureRefusesCurrencyMismatch) {
    auto user_id = seed_user("buyer3b@example.com");
    auto payment = seed_payment(user_id, "ORDER-3B", 1000, 1000);  // seeded currency is "USD"

    auto result = Billing::credit_capture("ORDER-3B", "CAPTURE-3B", 1000, "EUR");
    EXPECT_FALSE(result.credited);
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 0u);

    auto found = payments.find(payment.id);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "failed");
    ASSERT_TRUE(found->failure_reason.has_value());
    EXPECT_NE(found->failure_reason->find("currency mismatch"), std::string::npos);
}

TEST_F(WalletTest, CreditCaptureThrowsOnUnknownOrder) {
    EXPECT_THROW(Billing::credit_capture("NO-SUCH-ORDER", "CAPTURE-X", 100, "USD"), Repositories::PaymentNotFound);
}

// Pathological: the same capture id presented for a DIFFERENT order than the
// one that already recorded it trips the UNIQUE index, not the ordinary
// idempotent branch (that only fires for a same-order retry).
TEST_F(WalletTest, CreditCaptureThrowsDuplicateCaptureIdAcrossDifferentOrders) {
    auto user_id = seed_user("buyer3c@example.com");
    seed_payment(user_id, "ORDER-DUP-A", 1000, 1000);
    seed_payment(user_id, "ORDER-DUP-B", 500, 500);

    auto first = Billing::credit_capture("ORDER-DUP-A", "CAPTURE-SHARED", 1000, "USD");
    ASSERT_TRUE(first.credited);

    EXPECT_THROW(Billing::credit_capture("ORDER-DUP-B", "CAPTURE-SHARED", 500, "USD"), Billing::DuplicateCaptureId);
    // Order B was never touched.
    auto order_b = payments.find_by_order_id("ORDER-DUP-B");
    ASSERT_TRUE(order_b.has_value());
    EXPECT_EQ(order_b->status, "created");
    EXPECT_FALSE(order_b->provider_capture_id.has_value());
}

// ── refund_capture ───────────────────────────────────────────────────────────

TEST_F(WalletTest, RefundWritesNegativeEntry) {
    auto user_id = seed_user("buyer4@example.com");
    seed_payment(user_id, "ORDER-4", 1000, 1000);
    auto captured = Billing::credit_capture("ORDER-4", "CAPTURE-4", 1000, "USD");
    ASSERT_TRUE(captured.credited);
    ASSERT_EQ(captured.balance, 1000);

    auto refunded = Billing::refund_capture("CAPTURE-4", "REFUND-4", /*refunded_amount_cents=*/1000);
    EXPECT_TRUE(refunded.credited);
    EXPECT_EQ(refunded.balance, 0);
    EXPECT_EQ(Billing::balance_of(user_id), 0);

    auto hist = Billing::history(user_id, 10, 0);
    ASSERT_EQ(hist.size(), 2u);
    EXPECT_EQ(hist[0].kind, "refund");  // newest first
    EXPECT_EQ(hist[0].delta_credits, -1000);
    EXPECT_EQ(hist[0].reference, "REFUND-4");

    auto found = payments.find_by_capture_id("CAPTURE-4");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "refunded");

    // Idempotent: redelivering the SAME refund id is a no-op.
    auto redelivered = Billing::refund_capture("CAPTURE-4", "REFUND-4", 1000);
    EXPECT_FALSE(redelivered.credited);
    EXPECT_EQ(redelivered.balance, 0);
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 2u);
}

TEST_F(WalletTest, RefundCaptureThrowsOnUnknownCaptureId) {
    EXPECT_THROW(Billing::refund_capture("NO-SUCH-CAPTURE", "REFUND-X", 100), Repositories::PaymentNotFound);
}

// Idempotency is keyed on the refund id, not on payments.status — two
// DISTINCT partial refunds on the same capture must both apply, not collapse
// into one.
TEST_F(WalletTest, DistinctPartialRefundsBothApply) {
    auto user_id = seed_user("buyer4b@example.com");
    seed_payment(user_id, "ORDER-4B", 1000, 1000);
    auto captured = Billing::credit_capture("ORDER-4B", "CAPTURE-4B", 1000, "USD");
    ASSERT_TRUE(captured.credited);

    auto first_partial = Billing::refund_capture("CAPTURE-4B", "REFUND-4B-1", 400);
    EXPECT_TRUE(first_partial.credited);
    EXPECT_EQ(first_partial.balance, 600);

    // The cumulative total (400 so far) is still short of the full 1000, so
    // the payment stays 'captured' after just the first partial.
    auto after_first = payments.find_by_capture_id("CAPTURE-4B");
    ASSERT_TRUE(after_first.has_value());
    EXPECT_EQ(after_first->status, "captured");

    auto second_partial = Billing::refund_capture("CAPTURE-4B", "REFUND-4B-2", 600);
    EXPECT_TRUE(second_partial.credited);
    EXPECT_EQ(second_partial.balance, 0);

    EXPECT_EQ(Billing::balance_of(user_id), 0);
    auto hist = Billing::history(user_id, 10, 0);
    ASSERT_EQ(hist.size(), 3u);  // topup + 2 distinct refunds
    int refund_rows = 0;
    for (const auto& e : hist)
        if (e.kind == "refund")
            ++refund_rows;
    EXPECT_EQ(refund_rows, 2);

    // Status flips on the CUMULATIVE total reaching the full amount —
    // 400 + 600 == 1000, so once the second (distinct) partial lands, the
    // payment is 'refunded' even though neither individual call's own
    // amount equaled the full 1000.
    auto found = payments.find_by_capture_id("CAPTURE-4B");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "refunded");

    // Redelivering the FIRST partial refund's id again is still a no-op —
    // per-id idempotency, not a payments.status guard.
    auto redelivered_first = Billing::refund_capture("CAPTURE-4B", "REFUND-4B-1", 400);
    EXPECT_FALSE(redelivered_first.credited);
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 3u);
}

TEST_F(WalletTest, RefundCaptureRejectsInvalidAmounts) {
    auto user_id = seed_user("buyer4c@example.com");
    seed_payment(user_id, "ORDER-4C", 1000, 1000);
    Billing::credit_capture("ORDER-4C", "CAPTURE-4C", 1000, "USD");

    EXPECT_THROW(Billing::refund_capture("CAPTURE-4C", "REFUND-ZERO", 0), Billing::InvalidRefundAmount);
    EXPECT_THROW(Billing::refund_capture("CAPTURE-4C", "REFUND-NEG", -100), Billing::InvalidRefundAmount);
    EXPECT_THROW(Billing::refund_capture("CAPTURE-4C", "REFUND-OVER", 1001), Billing::InvalidRefundAmount);

    // Nothing was written by any of the three rejected calls — not even the
    // payment status.
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 1u);  // just the topup
    EXPECT_EQ(Billing::balance_of(user_id), 1000);
    auto found = payments.find_by_capture_id("CAPTURE-4C");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "captured");
}

// A refund must never be silently dropped even when it would drive the
// balance negative (the user already spent below the refund amount) — the
// spent-down state comes from the real Billing::spend writer.
TEST_F(WalletTest, RefundBeyondRemainingBalanceMarksPaymentRefundedWithoutGoingNegative) {
    auto user_id = seed_user("buyer5@example.com");
    seed_payment(user_id, "ORDER-5", 1000, 1000);
    auto captured = Billing::credit_capture("ORDER-5", "CAPTURE-5", 1000, "USD");
    ASSERT_TRUE(captured.credited);
    ASSERT_EQ(captured.balance, 1000);

    ASSERT_TRUE(Billing::spend(user_id, 900, "SPEND-DOWN-5").credited);
    ASSERT_EQ(Billing::balance_of(user_id), 100);

    auto refunded = Billing::refund_capture("CAPTURE-5", "REFUND-5", 1000);
    EXPECT_TRUE(refunded.credited);
    // The 1000-credit refund CANNOT apply (would drive balance to -900) — the
    // wallet is left exactly where the spend left it.
    EXPECT_EQ(refunded.balance, 100);
    EXPECT_EQ(Billing::balance_of(user_id), 100);

    auto found = payments.find_by_capture_id("CAPTURE-5");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "refunded");

    // No refund row was written — only the topup and the spend exist.
    auto hist = Billing::history(user_id, 10, 0);
    ASSERT_EQ(hist.size(), 2u);
    for (const auto& e : hist)
        EXPECT_NE(e.kind, "refund");

    // The attempt is durably recorded (billing_refunds), not the ledger.
    EXPECT_EQ(refund_outcome("REFUND-5"), "skipped_insufficient");

    // Redelivering the same refund id is now a genuine idempotent no-op —
    // the durable billing_refunds row is the marker, checked first.
    auto redelivered = Billing::refund_capture("CAPTURE-5", "REFUND-5", 1000);
    EXPECT_FALSE(redelivered.credited);
    EXPECT_EQ(redelivered.balance, 100);
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 2u);
}

// A refund small enough that it converts to 0 credits must never trip
// wallet_entries' CHECK(delta_credits <> 0) — it used to escape as an
// uncaught 500 that the provider would redeliver forever. Also: since this
// is a PARTIAL refund (50 cents of a 1000-cent payment), the payment status
// must stay 'captured', not flip to 'refunded'.
TEST_F(WalletTest, RefundSubUnitAmountConvertsToZeroCreditsIsRecordedNotCrashed) {
    auto user_id = seed_user("buyer5b@example.com");
    // 10 credits for the whole 1000-cent payment — 50 cents prorates to
    // (10 * 50) / 1000 = 0 credits (floors below 1 unit).
    seed_payment(user_id, "ORDER-5B", /*amount_cents=*/1000, /*credits_expected=*/10, /*rate_snapshot=*/1);
    auto captured = Billing::credit_capture("ORDER-5B", "CAPTURE-5B", 1000, "USD");
    ASSERT_TRUE(captured.credited);
    ASSERT_EQ(captured.balance, 10);

    auto result = Billing::refund_capture("CAPTURE-5B", "REFUND-SUBUNIT", /*refunded_amount_cents=*/50);
    EXPECT_TRUE(result.credited);
    EXPECT_EQ(result.balance, 10);  // untouched
    EXPECT_EQ(Billing::balance_of(user_id), 10);

    // No refund ledger row — just the original topup.
    auto hist = Billing::history(user_id, 10, 0);
    ASSERT_EQ(hist.size(), 1u);
    EXPECT_EQ(hist[0].kind, "topup");

    EXPECT_EQ(refund_outcome("REFUND-SUBUNIT"), "skipped_zero_credits");

    // Partial (50 != 1000) — status stays captured.
    auto found = payments.find_by_capture_id("CAPTURE-5B");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "captured");
}

// Regression test: refund_capture must prorate off credits_expected, NOT
// rate_snapshot. A bonus package sells 500 credits for only 400 cents
// (rate_snapshot frozen at the unrelated generic per-unit rate, 100 — i.e.
// "1 credit per cent" — which would derive only 400 credits from 400 cents
// if the old formula were still used). A FULL refund must deduct exactly the
// 500 credits that were actually granted, not 400 — proof that
// credit_capture and refund_capture agree on what a "full refund" means
// regardless of how the payment was priced.
TEST_F(WalletTest, RefundOfBonusPackagePurchaseDeductsExactCreditsGranted) {
    auto user_id = seed_user("buyer5g@example.com");
    seed_payment(user_id,
                 "ORDER-5G",
                 /*amount_cents=*/400,
                 /*credits_expected=*/500,
                 /*rate_snapshot=*/100);  // rate disagrees with credits_expected on purpose
    auto captured = Billing::credit_capture("ORDER-5G", "CAPTURE-5G", 400, "USD");
    ASSERT_TRUE(captured.credited);
    ASSERT_EQ(captured.balance, 500);

    auto refunded = Billing::refund_capture("CAPTURE-5G", "REFUND-5G", /*refunded_amount_cents=*/400);
    EXPECT_TRUE(refunded.credited);
    // A rate_snapshot-based conversion would have computed 400*100/100=400,
    // leaving the user with 100 credits after a "full" refund. The correct,
    // credits_expected-prorated answer deducts all 500.
    EXPECT_EQ(refunded.balance, 0);
    EXPECT_EQ(Billing::balance_of(user_id), 0);
    EXPECT_EQ(refund_outcome("REFUND-5G"), "applied");

    auto found = payments.find_by_capture_id("CAPTURE-5G");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "refunded");
}

// A normal (nonzero-credit, sufficient-balance) PARTIAL refund must not flip
// payments.status to 'refunded' — only reaching the full payment amount does
// that.
TEST_F(WalletTest, PartialRefundLeavesPaymentCaptured) {
    auto user_id = seed_user("buyer5c@example.com");
    seed_payment(user_id, "ORDER-5C", 1000, 1000);
    auto captured = Billing::credit_capture("ORDER-5C", "CAPTURE-5C", 1000, "USD");
    ASSERT_TRUE(captured.credited);

    auto result = Billing::refund_capture("CAPTURE-5C", "REFUND-5C", /*refunded_amount_cents=*/300);
    EXPECT_TRUE(result.credited);
    EXPECT_EQ(result.balance, 700);
    EXPECT_EQ(refund_outcome("REFUND-5C"), "applied");

    auto found = payments.find_by_capture_id("CAPTURE-5C");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "captured");  // NOT refunded — this was partial
}

// A refund against a payment that failed capture (amount/currency mismatch)
// must not overwrite 'failed' with 'refunded', even for an amount that
// matches the payment's amount_cents (the "full refund" condition on its own
// is not enough — the payment must also currently be 'captured').
TEST_F(WalletTest, RefundAgainstFailedPaymentDoesNotOverwriteStatus) {
    auto user_id = seed_user("buyer5d@example.com");
    seed_payment(user_id, "ORDER-5D", 1000, 1000);
    // Mismatched capture -> status='failed', but provider_capture_id is
    // still recorded (credit_capture sets it before validating the amount).
    auto mismatch = Billing::credit_capture("ORDER-5D", "CAPTURE-5D", /*captured_amount_cents=*/999, "USD");
    ASSERT_FALSE(mismatch.credited);
    auto pre = payments.find_by_capture_id("CAPTURE-5D");
    ASSERT_TRUE(pre.has_value());
    ASSERT_EQ(pre->status, "failed");

    auto result = Billing::refund_capture("CAPTURE-5D", "REFUND-5D", /*refunded_amount_cents=*/1000);
    EXPECT_TRUE(result.credited);  // recorded — nothing to refund since nothing was credited
    EXPECT_EQ(refund_outcome("REFUND-5D"), "skipped_insufficient");  // balance is 0

    auto found = payments.find_by_capture_id("CAPTURE-5D");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "failed");  // untouched
}

// Once a refund id is durably recorded as skipped (insufficient balance), a
// redelivery of that SAME id must stay a no-op forever — even if the wallet
// balance later recovers enough that the refund would now succeed. A delayed
// debit would double-process money the caller already considers refunded
// once.
TEST_F(WalletTest, DuplicateRefundIdAfterInsufficiencySkipNeverAppliesLater) {
    auto user_id = seed_user("buyer5e@example.com");
    seed_payment(user_id, "ORDER-5E", 1000, 1000);
    auto captured = Billing::credit_capture("ORDER-5E", "CAPTURE-5E", 1000, "USD");
    ASSERT_TRUE(captured.credited);

    // Spend down (via the real writer) so the (full) refund can't apply.
    ASSERT_TRUE(Billing::spend(user_id, 950, "SPEND-DOWN-5E").credited);
    ASSERT_EQ(Billing::balance_of(user_id), 50);

    auto first = Billing::refund_capture("CAPTURE-5E", "REFUND-RECOVER", 1000);
    EXPECT_TRUE(first.credited);
    EXPECT_EQ(first.balance, 50);
    EXPECT_EQ(refund_outcome("REFUND-RECOVER"), "skipped_insufficient");

    // "Recover" the balance well past what the refund would have needed.
    Billing::adjust(user_id, 5000, "top up for the test", seed_user("recover-admin@example.com"));
    ASSERT_EQ(Billing::balance_of(user_id), 5050);

    // Redelivering the SAME refund id must still be a no-op — no delayed
    // debit now that the balance could technically afford it.
    auto redelivered = Billing::refund_capture("CAPTURE-5E", "REFUND-RECOVER", 1000);
    EXPECT_FALSE(redelivered.credited);
    EXPECT_EQ(Billing::balance_of(user_id), 5050);     // untouched by the redelivery
    EXPECT_EQ(refund_row_count(first.payment_id), 1);  // still exactly one billing_refunds row
}

// The running total of every refund recorded against a payment must never
// exceed what was actually paid, even across separate calls that each
// individually look valid.
TEST_F(WalletTest, AggregateOverRefundIsRefused) {
    auto user_id = seed_user("buyer5f@example.com");
    seed_payment(user_id, "ORDER-5F", 1000, 1000);
    auto captured = Billing::credit_capture("ORDER-5F", "CAPTURE-5F", 1000, "USD");
    ASSERT_TRUE(captured.credited);

    auto first = Billing::refund_capture("CAPTURE-5F", "REFUND-AGG-1", 600);
    ASSERT_TRUE(first.credited);
    ASSERT_EQ(first.balance, 400);

    // 600 (already refunded) + 500 (this call) = 1100 > 1000 — individually
    // 500 <= amount_cents, so only the AGGREGATE check catches this.
    EXPECT_THROW(Billing::refund_capture("CAPTURE-5F", "REFUND-AGG-2", 500), Billing::InvalidRefundAmount);

    // The rejected call wrote nothing at all.
    EXPECT_EQ(Billing::balance_of(user_id), 400);
    EXPECT_EQ(refund_row_count(first.payment_id), 1);
    EXPECT_FALSE(refund_outcome("REFUND-AGG-2").has_value());
}

// ── adjust ───────────────────────────────────────────────────────────────────

TEST_F(WalletTest, AdjustWritesAuditedEntryAndMovesBalance) {
    auto user_id = seed_user("buyer6@example.com");
    auto admin_id = seed_user("admin1@example.com");

    auto result = Billing::adjust(user_id, 250, "goodwill credit", admin_id);
    EXPECT_TRUE(result.credited);
    EXPECT_EQ(result.balance, 250);

    auto hist = Billing::history(user_id, 10, 0);
    ASSERT_EQ(hist.size(), 1u);
    EXPECT_EQ(hist[0].kind, "adjustment");
    EXPECT_EQ(hist[0].delta_credits, 250);
    EXPECT_EQ(hist[0].note, "goodwill credit");
    ASSERT_TRUE(hist[0].created_by.has_value());
    EXPECT_EQ(*hist[0].created_by, admin_id);

    // A negative delta that would drive the balance below zero is refused
    // outright — nothing is applied (unlike a refund, no real money has
    // already moved for a manual adjustment).
    EXPECT_THROW(Billing::adjust(user_id, -1000, "oops", admin_id), Billing::InsufficientBalance);
    EXPECT_EQ(Billing::balance_of(user_id), 250);
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 1u);
}

TEST_F(WalletTest, AdjustRejectsZeroDelta) {
    auto user_id = seed_user("buyer6b@example.com");
    auto admin_id = seed_user("admin1b@example.com");

    EXPECT_THROW(Billing::adjust(user_id, 0, "noop", admin_id), Billing::ZeroAdjustment);
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 0u);
    EXPECT_EQ(Billing::balance_of(user_id), 0);
}

TEST_F(WalletTest, AdjustThrowsOnUnknownUser) {
    auto admin_id = seed_user("admin1c@example.com");
    // Syntactically valid UUID, but no such row.
    EXPECT_THROW(Billing::adjust("00000000-0000-0000-0000-000000000000", 10, "note", admin_id), Billing::UnknownUser);
}

TEST_F(WalletTest, AdjustThrowsOnMalformedUserId) {
    auto admin_id = seed_user("admin1d@example.com");
    EXPECT_THROW(Billing::adjust("not-a-uuid", 10, "note", admin_id), Billing::MalformedUserId);
}

// user_id and admin_id are both FKs on the same insert — a bad ADMIN id must
// be reported as such, not misattributed to user_id.
TEST_F(WalletTest, AdjustThrowsOnUnknownAdmin) {
    auto user_id = seed_user("buyer6c@example.com");
    EXPECT_THROW(Billing::adjust(user_id, 10, "note", "00000000-0000-0000-0000-000000000000"), Billing::UnknownAdmin);
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 0u);
}

TEST_F(WalletTest, AdjustThrowsOnMalformedAdminId) {
    auto user_id = seed_user("buyer6d@example.com");
    EXPECT_THROW(Billing::adjust(user_id, 10, "note", "not-a-uuid"), Billing::MalformedAdminId);
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 0u);
}

// ── spend ────────────────────────────────────────────────────────────────────

TEST_F(WalletTest, SpendWritesNegativeEntryAndMovesBalance) {
    auto user_id = seed_user("spender1@example.com");
    seed_payment(user_id, "ORDER-SP-1", 1000, 1000);
    ASSERT_TRUE(Billing::credit_capture("ORDER-SP-1", "CAP-SP-1", 1000, "USD").credited);

    auto result = Billing::spend(user_id, 300, "SPEND-1", "render job #42");
    EXPECT_TRUE(result.credited);
    EXPECT_EQ(result.balance, 700);
    EXPECT_EQ(Billing::balance_of(user_id), 700);

    auto hist = Billing::history(user_id, 10, 0);
    ASSERT_EQ(hist.size(), 2u);
    EXPECT_EQ(hist[0].kind, "spend");  // newest first
    EXPECT_EQ(hist[0].delta_credits, -300);
    EXPECT_EQ(hist[0].reference, "SPEND-1");
    EXPECT_EQ(hist[0].note, "render job #42");
}

TEST_F(WalletTest, SpendInsufficientCreditsWritesNoLedgerRow) {
    auto user_id = seed_user("spender2@example.com");
    seed_payment(user_id, "ORDER-SP-2", 100, 100);
    ASSERT_TRUE(Billing::credit_capture("ORDER-SP-2", "CAP-SP-2", 100, "USD").credited);

    EXPECT_THROW(Billing::spend(user_id, 500, "SPEND-TOO-BIG"), Billing::InsufficientCredits);

    // Nothing written — not a ledger row, not a balance change. And the
    // refused reference is NOT burned: unlike a skipped refund there is no
    // durable marker, so the same reference can succeed later once the
    // wallet can afford it.
    EXPECT_EQ(Billing::balance_of(user_id), 100);
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 1u);  // just the topup

    auto admin_id = seed_user("spend-admin@example.com");
    Billing::adjust(user_id, 1000, "top up for the test", admin_id);
    auto retry = Billing::spend(user_id, 500, "SPEND-TOO-BIG");
    EXPECT_TRUE(retry.credited);
    EXPECT_EQ(retry.balance, 600);
}

// A brand-new user (no wallet_balances row at all) refused for insufficiency
// must leave NO trace — the transaction rollback covers the materializing
// insert too.
TEST_F(WalletTest, SpendOnEmptyWalletRefusedWithoutMaterializingBalanceRow) {
    auto user_id = seed_user("spender2b@example.com");

    EXPECT_THROW(Billing::spend(user_id, 1, "SPEND-EMPTY"), Billing::InsufficientCredits);

    auto has_row = Database::get().execute_read([&](auto& txn) {
        auto r = txn.exec_params("SELECT 1 FROM wallet_balances WHERE user_id = $1", user_id);
        return !r.empty();
    });
    EXPECT_FALSE(has_row);
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 0u);
}

TEST_F(WalletTest, SpendIsIdempotentOnReference) {
    auto user_id = seed_user("spender3@example.com");
    seed_payment(user_id, "ORDER-SP-3", 1000, 1000);
    ASSERT_TRUE(Billing::credit_capture("ORDER-SP-3", "CAP-SP-3", 1000, "USD").credited);

    auto first = Billing::spend(user_id, 200, "SPEND-IDEM");
    ASSERT_TRUE(first.credited);
    ASSERT_EQ(first.balance, 800);

    // Same reference again — a retried charge — is a no-op reporting the
    // existing state, exactly one ledger row.
    auto retry = Billing::spend(user_id, 200, "SPEND-IDEM");
    EXPECT_FALSE(retry.credited);
    EXPECT_EQ(retry.balance, 800);

    // Idempotency is keyed on the reference alone (mirrors credit_capture's
    // per-capture-id idempotency): a retry that disagrees on the amount is
    // still the same charge — no second debit, no re-validation.
    auto retry_other_amount = Billing::spend(user_id, 999, "SPEND-IDEM");
    EXPECT_FALSE(retry_other_amount.credited);
    EXPECT_EQ(retry_other_amount.balance, 800);

    EXPECT_EQ(Billing::balance_of(user_id), 800);
    auto hist = Billing::history(user_id, 10, 0);
    ASSERT_EQ(hist.size(), 2u);  // topup + exactly one spend
}

// Two threads racing the SAME reference must produce exactly one ledger row
// and exactly one debit — the loser serializes on the balance-row lock and
// resolves at the under-lock idempotency check (see Billing::spend's docs).
TEST_F(WalletTest, ConcurrentDuplicateSpendDebitsExactlyOnce) {
    auto user_id = seed_user("spender4@example.com");
    seed_payment(user_id, "ORDER-SP-4", 1000, 1000);
    ASSERT_TRUE(Billing::credit_capture("ORDER-SP-4", "CAP-SP-4", 1000, "USD").credited);

    std::atomic<int> credited_count{0};
    std::atomic<int> failures{0};
    auto race = [&] {
        try {
            if (Billing::spend(user_id, 400, "SPEND-RACE").credited)
                ++credited_count;
        } catch (...) {
            ++failures;
        }
    };
    std::thread t1(race), t2(race);
    t1.join();
    t2.join();

    EXPECT_EQ(failures.load(), 0);        // both calls succeed — one applies, one no-ops
    EXPECT_EQ(credited_count.load(), 1);  // exactly one actually debited

    EXPECT_EQ(Billing::balance_of(user_id), 600);
    auto hist = Billing::history(user_id, 10, 0);
    ASSERT_EQ(hist.size(), 2u);  // topup + exactly ONE spend row
    int spend_rows = 0;
    for (const auto& e : hist)
        if (e.kind == "spend")
            ++spend_rows;
    EXPECT_EQ(spend_rows, 1);
}

TEST_F(WalletTest, SpendRejectsEmptyReference) {
    auto user_id = seed_user("spender5@example.com");
    seed_payment(user_id, "ORDER-SP-5", 1000, 1000);
    ASSERT_TRUE(Billing::credit_capture("ORDER-SP-5", "CAP-SP-5", 1000, "USD").credited);

    EXPECT_THROW(Billing::spend(user_id, 100, ""), Billing::MissingSpendReference);
    EXPECT_EQ(Billing::balance_of(user_id), 1000);
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 1u);
}

TEST_F(WalletTest, SpendRejectsNonPositiveAmount) {
    auto user_id = seed_user("spender5b@example.com");
    EXPECT_THROW(Billing::spend(user_id, 0, "SPEND-ZERO"), Billing::InvalidSpendAmount);
    // A negative "spend" must never silently CREDIT the wallet.
    EXPECT_THROW(Billing::spend(user_id, -50, "SPEND-NEG"), Billing::InvalidSpendAmount);
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 0u);
}

// A reference is an idempotency key, never a cross-wallet one: reusing user
// A's reference for user B is a conflict, not a silent no-op against the
// wrong wallet.
TEST_F(WalletTest, SpendRejectsReferenceReuseAcrossUsers) {
    auto user_a = seed_user("spender6a@example.com");
    auto user_b = seed_user("spender6b@example.com");
    seed_payment(user_a, "ORDER-SP-6A", 1000, 1000);
    seed_payment(user_b, "ORDER-SP-6B", 1000, 1000);
    ASSERT_TRUE(Billing::credit_capture("ORDER-SP-6A", "CAP-SP-6A", 1000, "USD").credited);
    ASSERT_TRUE(Billing::credit_capture("ORDER-SP-6B", "CAP-SP-6B", 1000, "USD").credited);

    ASSERT_TRUE(Billing::spend(user_a, 100, "SPEND-SHARED-REF").credited);

    EXPECT_THROW(Billing::spend(user_b, 100, "SPEND-SHARED-REF"), Billing::DuplicateSpendReference);
    EXPECT_EQ(Billing::balance_of(user_b), 1000);  // B untouched
    EXPECT_EQ(Billing::history(user_b, 10, 0).size(), 1u);
}

TEST_F(WalletTest, SpendThrowsOnUnknownUser) {
    EXPECT_THROW(Billing::spend("00000000-0000-0000-0000-000000000000", 10, "SPEND-NOUSER"), Billing::UnknownUser);
}

TEST_F(WalletTest, SpendThrowsOnMalformedUserId) {
    EXPECT_THROW(Billing::spend("not-a-uuid", 10, "SPEND-BADID"), Billing::MalformedUserId);
}

// ── the money invariant ──────────────────────────────────────────────────────

TEST_F(WalletTest, LedgerSumEqualsCachedBalanceAfterMixedTraffic) {
    auto user_a = seed_user("mix-a@example.com");
    auto user_b = seed_user("mix-b@example.com");
    auto admin_id = seed_user("admin2@example.com");

    seed_payment(user_a, "ORDER-A1", 1000, 1000);
    seed_payment(user_a, "ORDER-A2", 500, 500);
    seed_payment(user_b, "ORDER-B1", 2000, 2000);

    Billing::credit_capture("ORDER-A1", "CAP-A1", 1000, "USD");
    Billing::credit_capture("ORDER-A2", "CAP-A2", 500, "USD");
    Billing::credit_capture("ORDER-B1", "CAP-B1", 2000, "USD");
    Billing::refund_capture("CAP-A1", "REFUND-MIX-1", 1000);
    Billing::adjust(user_a, 50, "bonus", admin_id);
    Billing::adjust(user_b, -100, "correction", admin_id);
    Billing::spend(user_a, 200, "SPEND-MIX-1");
    Billing::spend(user_b, 500, "SPEND-MIX-2");
    // A duplicate capture attempt, a failed one, a redelivered refund id and
    // a retried spend reference — none of these should perturb the invariant.
    Billing::credit_capture("ORDER-A2", "CAP-A2", 500, "USD");
    Billing::refund_capture("CAP-A1", "REFUND-MIX-1", 1000);
    Billing::spend(user_a, 200, "SPEND-MIX-1");
    seed_payment(user_a, "ORDER-A3", 300, 300);
    Billing::credit_capture("ORDER-A3", "CAP-A3", 999, "USD");

    for (const auto& user_id : {user_a, user_b}) {
        auto hist = Billing::history(user_id, 100, 0);
        std::int64_t sum = 0;
        for (const auto& e : hist)
            sum += e.delta_credits;
        EXPECT_EQ(sum, Billing::balance_of(user_id)) << "user " << user_id;
    }
}

// Structural regression test for the lost-update fix in Wallet.hpp:
// `SELECT ... FOR UPDATE` cannot lock a `wallet_balances` row that doesn't
// exist yet, so a brand-new user's first pair of concurrent balance-changing
// writes used to both read `current = 0` unlocked, and the second writer's
// upsert would silently clobber the first's result — the cache disagreeing
// with SUM(wallet_entries.delta_credits) even though the ledger itself
// stayed correct (see the Wallet.hpp file comment for the full diagnosis).
// A genuine multi-threaded race isn't this codebase's test convention (see
// LedgerSumEqualsCachedBalanceAfterMixedTraffic above, which is sequential
// too) and isn't required here — the fix is structural (materialize the row
// before locking it, then a plain UPDATE instead of an upsert), so what
// matters is exercising that exact path — starting from NO wallet_balances
// row at all — and asserting the invariant holds afterward.
TEST_F(WalletTest, LedgerSumEqualsCachedBalanceForNewUserAfterCreditThenAdjust) {
    auto user_id = seed_user("new-user-race@example.com");
    auto admin_id = seed_user("admin-race@example.com");

    // Confirm there really is no wallet_balances row yet — the exact
    // condition that used to make the first `SELECT ... FOR UPDATE` lock
    // nothing.
    auto has_row = Database::get().execute_read([&](auto& txn) {
        auto r = txn.exec_params("SELECT 1 FROM wallet_balances WHERE user_id = $1", user_id);
        return !r.empty();
    });
    ASSERT_FALSE(has_row);

    seed_payment(user_id, "ORDER-NEW-1", 400, 400);
    auto credit_result = Billing::credit_capture("ORDER-NEW-1", "CAP-NEW-1", 400, "USD");
    ASSERT_TRUE(credit_result.credited);
    EXPECT_EQ(credit_result.balance, 400);

    auto adjust_result = Billing::adjust(user_id, 75, "welcome bonus", admin_id);
    ASSERT_TRUE(adjust_result.credited);
    EXPECT_EQ(adjust_result.balance, 475);

    auto hist = Billing::history(user_id, 10, 0);
    std::int64_t sum = 0;
    for (const auto& e : hist)
        sum += e.delta_credits;
    EXPECT_EQ(sum, Billing::balance_of(user_id));
    EXPECT_EQ(Billing::balance_of(user_id), 475);
}

// Randomized PROPERTY test, not an example test: ~300 pseudo-random
// credit/refund/adjust/spend operations with pseudo-random amounts, checking
// after every step that the two money invariants hold for the affected user:
//
//   1. SUM(wallet_entries.delta_credits) == wallet_balances.credits — the
//      append-only ledger and its cache never disagree;
//   2. wallet_balances.credits >= 0 — no operation mix can overdraw.
//
// Operations are allowed — expected — to be refused along the way
// (InsufficientCredits on a too-large spend, InsufficientBalance on a
// too-negative adjust, InvalidRefundAmount on a cumulative over-refund):
// refusals are part of the model, and the property is that a refusal
// perturbs NOTHING. Retried spend references (deliberately mixed in) must
// no-op rather than double-debit.
//
// Why hand-rolled rather than a property-testing library (rapidcheck &c.):
// a new vcpkg dependency invalidates the Dockerfile's dependency-install
// layer and rebuilds the entire ~29-package world, blowing CI timeouts
// (see the vcpkg.json "Don'ts" in CLAUDE.md) — rejected for what a seeded
// mt19937 loop covers just as well, minus automatic shrinking.
//
// Determinism: CONSTANT seed, and all randomness is derived from raw
// mt19937 output (whose sequence the C++ standard fully specifies) via
// modulo — never std::uniform_int_distribution, whose mapping is
// implementation-defined and would make failures irreproducible across
// stdlibs. A failure therefore replays exactly, on any platform, from the
// step number in the assertion message.
TEST_F(WalletTest, RandomizedMixedOperationsPreserveLedgerInvariants) {
    std::mt19937 rng(20260823u);  // constant seed — reproducible, see above
    auto pick = [&](std::uint32_t n) { return static_cast<std::int64_t>(rng() % n); };

    const std::vector<std::string> user_ids = {seed_user("rand-a@example.com"), seed_user("rand-b@example.com")};
    const auto admin_id = seed_user("rand-admin@example.com");

    struct CaptureRec {
        std::string capture_id;
        std::int64_t amount_cents;
    };
    std::vector<CaptureRec> captures;     // refund targets
    std::vector<std::string> spend_refs;  // for deliberate idempotent retries
    int order_seq = 0, refund_seq = 0, spend_seq = 0;
    int applied_spends = 0, refused_spends = 0;

    auto check_invariants = [&](const std::string& uid, int step) {
        auto hist = Billing::history(uid, 1000, 0);
        std::int64_t sum = 0;
        for (const auto& e : hist)
            sum += e.delta_credits;
        const auto cached = Billing::balance_of(uid);
        ASSERT_EQ(sum, cached) << "ledger/cache desync for user " << uid << " at step " << step;
        ASSERT_GE(cached, 0) << "negative balance for user " << uid << " at step " << step;
    };

    for (int step = 0; step < 300; ++step) {
        const auto& uid = user_ids[pick(2)];
        const auto op = pick(100);

        if (op < 30) {
            // Credit: a fresh order captured at 1 credit per cent.
            const std::int64_t amount = 1 + pick(2000);
            const std::string order_id = "RAND-ORDER-" + std::to_string(order_seq++);
            seed_payment(uid, order_id, amount, /*credits_expected=*/amount);
            const auto cap = Billing::credit_capture(order_id, "RAND-CAP-" + order_id, amount, "USD");
            ASSERT_TRUE(cap.credited) << "step " << step;
            captures.push_back({"RAND-CAP-" + order_id, amount});
        } else if (op < 60) {
            // Spend: usually a fresh reference, sometimes (~1 in 6) a retry
            // of an earlier one — which must no-op, never double-debit.
            std::string ref;
            if (!spend_refs.empty() && pick(6) == 0) {
                ref = spend_refs[pick(static_cast<std::uint32_t>(spend_refs.size()))];
            } else {
                ref = "RAND-SPEND-" + std::to_string(spend_seq++);
            }
            try {
                if (Billing::spend(uid, 1 + pick(1500), ref).credited) {
                    ++applied_spends;
                    spend_refs.push_back(ref);
                }
            } catch (const Billing::InsufficientCredits&) {
                ++refused_spends;  // expected: the wallet may well be short
            } catch (const Billing::DuplicateSpendReference&) {
                // expected: a retried reference may land on the OTHER user
            }
        } else if (op < 80 && !captures.empty()) {
            // Refund: a random capture, a random amount up to its full
            // value — the cumulative cap may refuse it, the wallet may be
            // short (skipped_insufficient), tiny amounts may floor to 0
            // credits: all are part of the model.
            const auto& cap = captures[pick(static_cast<std::uint32_t>(captures.size()))];
            const std::int64_t amount = 1 + pick(static_cast<std::uint32_t>(cap.amount_cents));
            try {
                Billing::refund_capture(cap.capture_id, "RAND-REFUND-" + std::to_string(refund_seq++), amount);
            } catch (const Billing::InvalidRefundAmount&) {
                // expected: cumulative refunds for this capture exceeded it
            }
        } else {
            // Adjust: -500..+500, never 0.
            std::int64_t delta = pick(500) + 1;
            if (pick(2) == 0)
                delta = -delta;
            try {
                Billing::adjust(uid, delta, "random adjustment", admin_id);
            } catch (const Billing::InsufficientBalance&) {
                // expected: a negative delta the wallet can't absorb
            }
        }

        check_invariants(uid, step);
        if (::testing::Test::HasFatalFailure())
            return;  // stop at the FIRST violating step — that's the repro
    }

    for (const auto& uid : user_ids)
        check_invariants(uid, /*step=*/300);

    // Sanity that the mix actually exercised the writer under test in both
    // directions — some spends applied AND some were refused (deterministic
    // given the constant seed; this documents that the distribution isn't
    // degenerate, not a tuning knob).
    EXPECT_GT(applied_spends, 0);
    EXPECT_GT(refused_spends, 0);
}

// ── UserRepository::remove() vs. billing history ────────────────────────────

TEST_F(WalletTest, DeletingUserWithWalletHistoryIsBlocked) {
    auto user_id = seed_user("has-history@example.com");
    seed_payment(user_id, "ORDER-DEL-1", 1000, 1000);
    auto result = Billing::credit_capture("ORDER-DEL-1", "CAPTURE-DEL-1", 1000, "USD");
    ASSERT_TRUE(result.credited);

    EXPECT_THROW(users.remove(user_id), Repositories::UserHasBillingHistory);
    // Not deleted — the typed error means the transaction rolled back cleanly.
    EXPECT_TRUE(users.find(user_id).has_value());
}

TEST_F(WalletTest, DeletingUserWithoutBillingHistorySucceeds) {
    auto user_id = seed_user("no-history@example.com");
    EXPECT_NO_THROW(users.remove(user_id));
    EXPECT_FALSE(users.find(user_id).has_value());
}

// ── Repositories::PackageRepository ─────────────────────────────────────────

TEST_F(WalletTest, PackageRepositoryCreateAndFindRoundtrip) {
    auto created = packages.create("Starter Pack", /*amount_cents=*/500, /*credits=*/500, /*active=*/true, /*sort=*/1);
    EXPECT_EQ(created.title, "Starter Pack");
    EXPECT_EQ(created.amount_cents, 500);
    EXPECT_EQ(created.credits, 500);
    EXPECT_TRUE(created.active);
    EXPECT_EQ(created.sort, 1);

    auto found = packages.find(created.id);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->title, "Starter Pack");
}

TEST_F(WalletTest, PackageRepositoryListActiveExcludesInactiveAndOrdersBySort) {
    packages.create("Second", 1000, 1000, /*active=*/true, /*sort=*/2);
    packages.create("First", 500, 500, /*active=*/true, /*sort=*/1);
    packages.create("Hidden", 2000, 2000, /*active=*/false, /*sort=*/0);

    auto active = packages.list_active();
    ASSERT_EQ(active.size(), 2u);
    EXPECT_EQ(active[0].title, "First");  // sort=1 before sort=2
    EXPECT_EQ(active[1].title, "Second");
    for (const auto& p : active)
        EXPECT_NE(p.title, "Hidden");
}

TEST_F(WalletTest, PackageRepositoryUpdateKeepsOmittedFields) {
    auto created = packages.create("Original", 500, 500, true, 1);

    auto updated = packages.update(created.id,
                                   /*title=*/std::string("Renamed"),
                                   /*amount_cents=*/std::nullopt,
                                   /*credits=*/std::nullopt,
                                   /*active=*/false,
                                   /*sort=*/std::nullopt);
    EXPECT_EQ(updated.title, "Renamed");
    EXPECT_EQ(updated.amount_cents, 500);  // unchanged
    EXPECT_EQ(updated.credits, 500);       // unchanged
    EXPECT_FALSE(updated.active);
    EXPECT_EQ(updated.sort, 1);  // unchanged

    EXPECT_THROW(packages.update("00000000-0000-0000-0000-000000000000",
                                 std::string("x"),
                                 std::nullopt,
                                 std::nullopt,
                                 std::nullopt,
                                 std::nullopt),
                 Repositories::PackageNotFound);
}

TEST_F(WalletTest, PackageRepositoryRemoveDeletesRow) {
    auto created = packages.create("Removable", 500, 500, true, 1);
    packages.remove(created.id);
    EXPECT_FALSE(packages.find(created.id).has_value());
    EXPECT_THROW(packages.remove(created.id), Repositories::PackageNotFound);
}

// ── Repositories::PaymentRepository::mark_approved ──────────────────────────

TEST_F(WalletTest, MarkApprovedTransitionsFromCreated) {
    auto user_id = seed_user("approver@example.com");
    seed_payment(user_id, "ORDER-APPROVE-1", 1000, 1000);

    EXPECT_TRUE(payments.mark_approved("ORDER-APPROVE-1"));
    auto found = payments.find_by_order_id("ORDER-APPROVE-1");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "approved");
}

TEST_F(WalletTest, MarkApprovedIsNoOpAfterCapture) {
    auto user_id = seed_user("approver2@example.com");
    seed_payment(user_id, "ORDER-APPROVE-2", 1000, 1000);
    Billing::credit_capture("ORDER-APPROVE-2", "CAPTURE-APPROVE-2", 1000, "USD");

    // A duplicate/out-of-order APPROVED webhook after capture must not throw
    // (that would make the provider redeliver it forever) — just report false.
    EXPECT_FALSE(payments.mark_approved("ORDER-APPROVE-2"));
    auto found = payments.find_by_order_id("ORDER-APPROVE-2");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "captured");  // unchanged
}

TEST_F(WalletTest, MarkApprovedThrowsOnUnknownOrder) {
    EXPECT_THROW(payments.mark_approved("NO-SUCH-ORDER"), Repositories::PaymentNotFound);
}

// ── Repositories::BillingSettingsRepository ─────────────────────────────────

// The single row is seeded by migration 008 and reset to the defaults by this
// suite's SetUp/TearDown (UPDATE, never TRUNCATE — the repository assumes the
// row always exists), so this test can both trust the starting values and
// mutate them freely.
TEST_F(WalletTest, BillingSettingsSeededDefaultsAndUpdateRoundtrip) {
    auto before = settings.get();
    EXPECT_EQ(before.credits_per_unit, 100);
    EXPECT_EQ(before.min_amount_cents, 100);
    EXPECT_EQ(before.max_amount_cents, 100000);

    auto updated = settings.update(/*credits_per_unit=*/250, /*min_amount_cents=*/200, /*max_amount_cents=*/50000);
    EXPECT_EQ(updated.credits_per_unit, 250);
    EXPECT_EQ(updated.min_amount_cents, 200);
    EXPECT_EQ(updated.max_amount_cents, 50000);

    // Read-back agrees, including the primary-forcing variant.
    auto after = settings.get(/*from_primary=*/true);
    EXPECT_EQ(after.credits_per_unit, 250);
    EXPECT_EQ(after.min_amount_cents, 200);
    EXPECT_EQ(after.max_amount_cents, 50000);
}

}  // namespace
