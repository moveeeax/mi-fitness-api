-- Migration 015: drop_account_flow_tables
--
-- The template's email module and the account flows behind it (confirm,
-- reset-password, change-email, invite) are removed by the repo audit
-- (2026-09-30): SMTP was never configured here and the routes were dead.
-- used_tokens (migration 002) held their one-shot token nonces; outbox
-- (migration 010) relayed their emails. Both tables sit empty.
--
-- The MigrationRunner wraps this file in ONE transaction. No BEGIN/COMMIT.

DROP TABLE IF EXISTS used_tokens;
DROP TABLE IF EXISTS outbox;
