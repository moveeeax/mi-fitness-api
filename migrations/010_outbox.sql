-- Migration 010: transactional outbox — opt-in atomic event dispatch.
--
-- Applied in numeric order on boot. The MigrationRunner wraps this file in ONE
-- transaction under an advisory lock — do NOT add BEGIN/COMMIT. Idempotent DDL.
--
-- The after-response hook (HandlerSupport.hpp 4-arg with_repo_errors) fires a
-- side effect AFTER the DB commit: a process that dies between the commit and
-- Jobs::submit loses the event forever. For events that must not be lost
-- (money mail, webhooks about paid state), the row is written HERE, inside the
-- SAME transaction as the domain write, and a periodic drain task relays it to
-- the Redis job queue afterwards (src/jobs/Outbox.hpp). Commit → the event is
-- durable; rollback → the event never existed. Delivery is at-least-once.
--
-- Columns:
--   kind        the Jobs type Outbox::drain() will submit ("email.send",
--               "webhook.deliver", any registered handler type)
--   payload     the exact Jobs::submit payload, opaque to the outbox
--   claimed_at  NULL = ready for drain; non-NULL = a drainer claimed it
--               (re-claimable after Outbox::kStaleClaimSec so a drainer that
--               died between claim and submit can't strand the row forever)
--   attempts    failed submit attempts so far (grows without bound — retry
--               forever is correct for "must not be lost"; alert on it)
--   last_error  what the most recent failed submit threw
CREATE TABLE IF NOT EXISTS outbox (
    id          UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    kind        TEXT        NOT NULL,
    payload     JSONB       NOT NULL DEFAULT '{}'::jsonb,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    claimed_at  TIMESTAMPTZ,
    attempts    INTEGER     NOT NULL DEFAULT 0,
    last_error  TEXT        NOT NULL DEFAULT ''
);

-- Drain order is FIFO over the unclaimed backlog; the partial index keeps the
-- claim query cheap no matter how many claimed-in-flight rows exist.
CREATE INDEX IF NOT EXISTS idx_outbox_unclaimed
    ON outbox (created_at)
    WHERE claimed_at IS NULL;
