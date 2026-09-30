-- Migration 012: health_data
-- Created: 2026-09-30T01:37:50Z
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

-- Восемь таблиц данных здоровья плюс состояние и журнал синка. Схема повторяет
-- SQLite Python-моста по составу полей (docs спеки, раздел «Схема Postgres»),
-- но с естественными ключами уникальности вместо строкового id.
--
-- Общие правила:
--   * device_id NOT NULL DEFAULT '': он входит в ключи уникальности, а NULL
--     в уникальном индексе Postgres не сравнивается с NULL.
--   * Отсутствующие измерения это NULL, не ноль: ноль от сервера означает
--     «нет данных» (правило 2 нормализации).

CREATE TABLE IF NOT EXISTS daily_activity (
    id                bigserial   PRIMARY KEY,
    provider          text        NOT NULL DEFAULT 'mi_fitness',
    source_type       text        NOT NULL DEFAULT 'cloud_session',
    source_record_id  text,
    user_id           text        NOT NULL,
    device_id         text        NOT NULL DEFAULT '',
    timezone          text        NOT NULL DEFAULT 'UTC',
    collected_at      timestamptz,
    created_at        timestamptz NOT NULL DEFAULT now(),
    updated_at        timestamptz NOT NULL DEFAULT now(),
    date              date        NOT NULL,
    steps             integer     NOT NULL DEFAULT 0,
    distance_m        double precision,
    active_kcal       double precision,
    total_kcal        double precision,
    floors            integer,
    active_minutes    integer,
    UNIQUE (user_id, date, device_id)
);

CREATE TABLE IF NOT EXISTS sleep_sessions (
    id                  bigserial   PRIMARY KEY,
    provider            text        NOT NULL DEFAULT 'mi_fitness',
    source_type         text        NOT NULL DEFAULT 'cloud_session',
    source_record_id    text,
    user_id             text        NOT NULL,
    device_id           text        NOT NULL DEFAULT '',
    timezone            text        NOT NULL DEFAULT 'UTC',
    collected_at        timestamptz,
    created_at          timestamptz NOT NULL DEFAULT now(),
    updated_at          timestamptz NOT NULL DEFAULT now(),
    sleep_id            text        NOT NULL,
    start_at            timestamptz NOT NULL,
    end_at              timestamptz NOT NULL,
    duration_minutes    integer     NOT NULL,
    time_asleep_minutes integer     NOT NULL,
    time_awake_minutes  integer     NOT NULL,
    sleep_score         integer,
    sleep_score_source  text,
    is_nap              boolean     NOT NULL DEFAULT false,
    stages              jsonb       NOT NULL DEFAULT '[]',
    UNIQUE (user_id, sleep_id)
);
CREATE INDEX IF NOT EXISTS idx_sleep_user_start ON sleep_sessions (user_id, start_at);

CREATE TABLE IF NOT EXISTS workouts (
    id                  bigserial   PRIMARY KEY,
    provider            text        NOT NULL DEFAULT 'mi_fitness',
    source_type         text        NOT NULL DEFAULT 'cloud_session',
    source_record_id    text,
    user_id             text        NOT NULL,
    device_id           text        NOT NULL DEFAULT '',
    timezone            text        NOT NULL DEFAULT 'UTC',
    collected_at        timestamptz,
    created_at          timestamptz NOT NULL DEFAULT now(),
    updated_at          timestamptz NOT NULL DEFAULT now(),
    workout_id          text        NOT NULL,
    activity_type       text        NOT NULL,
    start_at            timestamptz NOT NULL,
    end_at              timestamptz NOT NULL,
    duration_minutes    integer     NOT NULL,
    distance_m          double precision,
    calories_kcal       double precision,
    avg_heart_rate_bpm  integer,
    max_heart_rate_bpm  integer,
    avg_pace_sec_per_km double precision,
    max_pace_sec_per_km double precision,
    total_steps         integer,
    UNIQUE (user_id, workout_id)
);

CREATE TABLE IF NOT EXISTS body_measurements (
    id                    bigserial   PRIMARY KEY,
    provider              text        NOT NULL DEFAULT 'mi_fitness',
    source_type           text        NOT NULL DEFAULT 'cloud_session',
    source_record_id      text,
    user_id               text        NOT NULL,
    device_id             text        NOT NULL DEFAULT '',
    timezone              text        NOT NULL DEFAULT 'UTC',
    collected_at          timestamptz,
    created_at            timestamptz NOT NULL DEFAULT now(),
    updated_at            timestamptz NOT NULL DEFAULT now(),
    timestamp             timestamptz NOT NULL,
    weight_kg             double precision NOT NULL,
    bmi                   double precision,
    body_fat_pct          double precision,
    muscle_mass_kg        double precision,
    water_pct             double precision,
    bone_mass_kg          double precision,
    visceral_fat_score    integer,
    basal_metabolism_kcal integer,
    metabolic_age         integer,
    UNIQUE (user_id, timestamp, device_id)
);

CREATE TABLE IF NOT EXISTS heart_rate_samples (
    id               bigserial   PRIMARY KEY,
    provider         text        NOT NULL DEFAULT 'mi_fitness',
    source_type      text        NOT NULL DEFAULT 'cloud_session',
    source_record_id text,
    user_id          text        NOT NULL,
    device_id        text        NOT NULL DEFAULT '',
    timezone         text        NOT NULL DEFAULT 'UTC',
    collected_at     timestamptz,
    created_at       timestamptz NOT NULL DEFAULT now(),
    updated_at       timestamptz NOT NULL DEFAULT now(),
    timestamp        timestamptz NOT NULL,
    bpm              integer     NOT NULL,
    sample_type      text        NOT NULL,
    UNIQUE (user_id, timestamp, sample_type)
);

CREATE TABLE IF NOT EXISTS spo2_samples (
    id               bigserial   PRIMARY KEY,
    provider         text        NOT NULL DEFAULT 'mi_fitness',
    source_type      text        NOT NULL DEFAULT 'cloud_session',
    source_record_id text,
    user_id          text        NOT NULL,
    device_id        text        NOT NULL DEFAULT '',
    timezone         text        NOT NULL DEFAULT 'UTC',
    collected_at     timestamptz,
    created_at       timestamptz NOT NULL DEFAULT now(),
    updated_at       timestamptz NOT NULL DEFAULT now(),
    timestamp        timestamptz NOT NULL,
    spo2_pct         integer     NOT NULL,
    UNIQUE (user_id, timestamp)
);

CREATE TABLE IF NOT EXISTS stress_samples (
    id               bigserial   PRIMARY KEY,
    provider         text        NOT NULL DEFAULT 'mi_fitness',
    source_type      text        NOT NULL DEFAULT 'cloud_session',
    source_record_id text,
    user_id          text        NOT NULL,
    device_id        text        NOT NULL DEFAULT '',
    timezone         text        NOT NULL DEFAULT 'UTC',
    collected_at     timestamptz,
    created_at       timestamptz NOT NULL DEFAULT now(),
    updated_at       timestamptz NOT NULL DEFAULT now(),
    timestamp        timestamptz NOT NULL,
    stress_score     integer     NOT NULL,
    level            text        NOT NULL,
    UNIQUE (user_id, timestamp)
);

CREATE TABLE IF NOT EXISTS abnormal_heart_beat_events (
    id               bigserial   PRIMARY KEY,
    provider         text        NOT NULL DEFAULT 'mi_fitness',
    source_type      text        NOT NULL DEFAULT 'cloud_session',
    source_record_id text,
    user_id          text        NOT NULL,
    device_id        text        NOT NULL DEFAULT '',
    timezone         text        NOT NULL DEFAULT 'UTC',
    collected_at     timestamptz,
    created_at       timestamptz NOT NULL DEFAULT now(),
    updated_at       timestamptz NOT NULL DEFAULT now(),
    event_id         text        NOT NULL,
    start_at         timestamptz NOT NULL,
    end_at           timestamptz NOT NULL,
    duration_seconds integer,
    UNIQUE (user_id, event_id)
);

CREATE TABLE IF NOT EXISTS sync_state (
    data_type             text        PRIMARY KEY,
    last_sync_at          timestamptz,
    last_record_timestamp timestamptz,
    records_count         bigint      NOT NULL DEFAULT 0
);

-- Журнал запусков синка: один запуск на систему одновременно (advisory lock),
-- статусы running / succeeded / failed / interrupted / skipped, результат
-- по типам данных в jsonb.
CREATE TABLE IF NOT EXISTS sync_runs (
    id              bigserial   PRIMARY KEY,
    started_at      timestamptz NOT NULL DEFAULT now(),
    finished_at     timestamptz,
    status          text        NOT NULL DEFAULT 'running',
    requested_start date,
    requested_end   date,
    data_types      text[]      NOT NULL DEFAULT '{}',
    result          jsonb       NOT NULL DEFAULT '{}'
);
