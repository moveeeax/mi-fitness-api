-- Migration 016: health_read_indexes
--
-- Маршруты чтения фильтруют сэмплы полуинтервалом по времени. Ключи
-- уникальности начинаются с user_id и диапазону по одному времени не
-- служат; на квартале это seq scan по десяткам тысяч строк на запрос.
--
-- The MigrationRunner wraps this file in ONE transaction. No BEGIN/COMMIT.

CREATE INDEX IF NOT EXISTS idx_heart_rate_time ON heart_rate_samples (timestamp);
CREATE INDEX IF NOT EXISTS idx_stress_time ON stress_samples (timestamp);
CREATE INDEX IF NOT EXISTS idx_spo2_time ON spo2_samples (timestamp);
CREATE INDEX IF NOT EXISTS idx_body_time ON body_measurements (timestamp);
CREATE INDEX IF NOT EXISTS idx_workouts_start ON workouts (start_at);
CREATE INDEX IF NOT EXISTS idx_sleep_end ON sleep_sessions (end_at);
CREATE INDEX IF NOT EXISTS idx_activity_date ON daily_activity (date);
