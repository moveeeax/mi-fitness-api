-- Migration 014: drop_billing
--
-- The billing module is removed from the service (repo audit, 2026-09-30):
-- the wallet was never funded and no payment ever existed in this
-- deployment. Tables from migrations 007-009 are dropped; those migration
-- files stay untouched — schema_migrations already records them.
--
-- The MigrationRunner wraps this file in ONE transaction. No BEGIN/COMMIT.

DROP TABLE IF EXISTS billing_refunds;
DROP TABLE IF EXISTS wallet_balances;
DROP TABLE IF EXISTS wallet_entries;
DROP TABLE IF EXISTS payments;
DROP TABLE IF EXISTS billing_packages;
DROP TABLE IF EXISTS billing_settings;
