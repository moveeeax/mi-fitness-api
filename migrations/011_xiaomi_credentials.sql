-- Migration 011: xiaomi_credentials
-- Created: 2026-09-29T13:40:17Z
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

-- Живой passToken аккаунта Xiaomi. Xiaomi ротирует его при каждом логине, и
-- предыдущее значение через какое-то время перестаёт работать, поэтому Secret
-- кластера хранит только стартовое значение (сид), а актуальное живёт здесь.
--
-- Токен лежит зашифрованным: crypto_secretbox_easy из libsodium, ключ приходит
-- из переменной окружения MI_FITNESS_TOKEN_KEY и в базу не попадает никогда.
-- Шифротекст и nonce хранятся в TEXT как base64, а не в BYTEA: во всём проекте
-- нет ни одной двоичной колонки, и заводить первую ради 350 байт незачем.
--
-- Строка ровно одна: сервис обслуживает один аккаунт (см. спеку, решение 2).
CREATE TABLE IF NOT EXISTS xiaomi_credentials (
    user_id           TEXT        PRIMARY KEY,
    pass_token_sealed TEXT        NOT NULL,  -- base64(crypto_secretbox_easy(...))
    nonce             TEXT        NOT NULL,  -- base64, 24 байта
    region            TEXT        NOT NULL DEFAULT 'cn',
    rotated_at        TIMESTAMPTZ NOT NULL DEFAULT now()
);

COMMENT ON TABLE xiaomi_credentials IS
    'Живой passToken Xiaomi: ротируется при каждом логине, Secret хранит только сид.';
