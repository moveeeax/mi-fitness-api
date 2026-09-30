-- Migration 013: sync_run_mutex
-- Created: 2026-09-30T03:08:27Z
--
-- Migrations are applied in numeric order on app boot (or via
-- RUN_MIGRATIONS_ONLY=1 ./mi_fitness_api). Use --verify-migrations to
-- list pending without applying.
--
-- The MigrationRunner already wraps this file in ONE transaction (under an
-- advisory lock) together with the schema_migrations bookkeeping. Do NOT add
-- BEGIN/COMMIT — an embedded COMMIT ends that transaction early and breaks
-- atomicity. Prefer idempotent DDL (IF NOT EXISTS / ON CONFLICT DO NOTHING).

-- TODO: write your DDL here. Common table skeleton (uncomment + rename):
--
-- CREATE TABLE IF NOT EXISTS your_table (
--     id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
--     name       TEXT        NOT NULL,
--     created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
--     updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
-- );
--
-- DROP TRIGGER IF EXISTS your_table_touch_updated_at ON your_table;
-- CREATE TRIGGER your_table_touch_updated_at
--     BEFORE UPDATE ON your_table
--     FOR EACH ROW EXECUTE FUNCTION touch_updated_at();

-- Взаимное исключение синка: единственная строка running на всю систему.
-- Advisory lock не подходит: он сессионный, а пул соединений отдаёт разные
-- сессии на каждый вызов. Частичный уникальный индекс делает переход
-- queued -> running атомарным, второй одновременный запуск ловит нарушение
-- уникальности и честно завершается статусом skipped.
CREATE UNIQUE INDEX IF NOT EXISTS one_running_sync_run
    ON sync_runs ((1))
    WHERE status = 'running';
