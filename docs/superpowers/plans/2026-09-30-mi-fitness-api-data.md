# Mi Fitness API: данные (нормализация и синк) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Восемь типов данных Mi Fitness нормализуются по правилам эталона, ложатся в Postgres идемпотентно, синк живёт в воркере с журналом запусков, числа сверены с Python-мостом.

**Architecture:** `Normalize` это чистые функции сырой JSON → доменные структуры, без сети и базы. Репозитории делают upsert через `ON CONFLICT` с различением вставки и обновления по `xmax = 0`. `SyncService` режет диапазон на куски, ведёт счётчики и журнал `sync_runs`, один запуск на систему через advisory lock Postgres. Воркер обрабатывает задание `xiaomi_sync` из очереди шаблона, API ставит задания маршрутом `POST /api/v1/sync`.

**Tech Stack:** как в плане 1. Эталон семантики: `mi_fitness_data_bridge/src/mi_fitness_mcp/adapters/mi_fitness_cloud.py` и `services/sync_service.py`, эталонные числа: его SQLite на PVC namespace `mi-fitness`.

**Spec:** `docs/superpowers/specs/2026-09-29-mi-fitness-api-cpp-design.md`

## Global Constraints

Все ограничения плана 1 действуют, включая запрет локальной сборки (CLAUDE.md) и два прогона CI на задачу. Дополнительно:

1. Семь правил нормализации из спеки это вопрос корректности данных, каждое закрывается своим тестом. Дедуп шагов: максимум по кортежу (шаги, дистанция, калории) в паре «локальная дата, локальная минута», не сумма. Ноль от сервера это NULL. Оценка сна не выдумывается и не размазывается.
2. Сырые записи всех ключей, кроме тренировок, приходят как `{time, zone_offset, zone_name, sid, value: "<json-строка>"}`; `value` бывает и объектом. Метки до 2000 года отбрасываются как мусор. Битые записи пропускаются со счётчиком skipped, никогда не роняют тип целиком.
3. Тренировки идут другим эндпоинтом: `/app/v1/data/get_sport_records_by_time`, `limit: 50`, поле `sport_records`. Суточные отчёты сна: `/app/v1/data/get_aggregated_fitness_data_by_time`, `key: sleep, tag: daily_report, limit: 100`, курсор строго строковый и непустой.
4. Никакой диагностики логином мимо сервиса: каждый логин ротирует токен. Пока probe и синк логинятся независимо, их нельзя гонять одновременно; объединение сессии это план 3.
5. Идентификаторы записей: естественные ключи уникальности из спеки, `device_id` в них NOT NULL DEFAULT ''. Вставка и обновление различаются `RETURNING (xmax = 0)`.
6. Значения из `value`-строк не попадают в логи: это данные здоровья.

## Review Focus

1. Повторный синк того же диапазона: added 0, updated N, строки не множатся ни в одной из восьми таблиц. Тест в задаче 5 гоняет два прохода на одних фикстурах.
2. Ноль от сервера уходит в NULL по каждому optional-полю каждого типа: тесты задач 2-4 кормят нормализацию нулями и проверяют NULL в базе, а не 0.
3. Две записи шагов в одну локальную минуту от разных устройств дают максимум, подавленные шаги считаются в отчёте синка. Тест в задаче 2.
4. Неоднозначные кандидаты на суточную оценку сна не получают её; повторный синк при недоступном отчёте не стирает ранее известную оценку тех же границ (COALESCE в upsert). Тесты в задаче 3.
5. Провал одного типа не отменяет остальные, `sync_runs.result` несёт счётчики и класс ошибки по каждому типу; auth-ошибка помечает запуск требующим свежего токена. Тест в задаче 5.

---

### Task 1: Схема данных и журнал запусков

**Files:**
- Create: `migrations/012_health_data.sql` (через `make new-migration SLUG=health_data`)
- Create: `src/domain/Health.hpp` (структуры восьми типов, std и nlohmann только)
- Create: `src/repositories/SyncRunRepository.hpp`
- Create: `tests/integration/test_health_schema.cpp`

**Interfaces:**
- Consumes: миграционный раннер шаблона
- Produces: таблицы `daily_activity, sleep_sessions, workouts, body_measurements, heart_rate_samples, spo2_samples, stress_samples, abnormal_heart_beat_events, sync_state, sync_runs`; структуры `Domain::DailyActivity, SleepSession, SleepStage, Workout, BodyMeasurement, HeartRateSample, Spo2Sample, StressSample, AbnormalHeartBeatEvent`; `Repositories::SyncRunRepository` с `long create(from, to, types)`, `void finish(id, status, result_json)`, `std::optional<nlohmann::json> get(id)`; константа `kSyncAdvisoryLockKey`

- [ ] **Step 1: Миграция**

Колонки по таблице спеки (раздел «Схема Postgres»), общая часть у всех: `provider text NOT NULL DEFAULT 'mi_fitness'`, `source_type text NOT NULL DEFAULT 'cloud_session'`, `source_record_id text`, `user_id text NOT NULL`, `device_id text NOT NULL DEFAULT ''`, `timezone text NOT NULL DEFAULT 'UTC'`, `collected_at timestamptz`, `created_at timestamptz NOT NULL DEFAULT now()`, `updated_at timestamptz NOT NULL DEFAULT now()`. Ключи уникальности дословно из спеки: `(user_id, date, device_id)`, `(user_id, sleep_id)`, `(user_id, workout_id)`, `(user_id, timestamp, device_id)`, `(user_id, timestamp, sample_type)`, `(user_id, timestamp)` дважды, `(user_id, event_id)`. `sleep_sessions.stages jsonb NOT NULL DEFAULT '[]'`. `sync_runs`: `id bigserial PRIMARY KEY, started_at timestamptz NOT NULL DEFAULT now(), finished_at timestamptz, status text NOT NULL DEFAULT 'running', requested_start date, requested_end date, data_types text[], result jsonb NOT NULL DEFAULT '{}'`. `sync_state`: `data_type text PRIMARY KEY, last_sync_at timestamptz, last_record_timestamp timestamptz, records_count bigint NOT NULL DEFAULT 0`.

- [ ] **Step 2: Падающий интеграционный тест**

```cpp
/**
 * @file test_health_schema.cpp
 * @brief Таблицы данных существуют, ключи уникальности работают, журнал пишет.
 */
TEST_F(HealthSchemaTest, AllTablesExistWithUniqueKeys) {
    for (const char* table :
         {"daily_activity", "sleep_sessions", "workouts", "body_measurements",
          "heart_rate_samples", "spo2_samples", "stress_samples",
          "abnormal_heart_beat_events", "sync_state", "sync_runs"}) {
        EXPECT_NO_THROW(count_rows(table)) << table;
    }
}

TEST_F(HealthSchemaTest, DuplicateNaturalKeyIsRejectedWithoutOnConflict) {
    insert_activity("2026-09-22", 100);
    EXPECT_THROW(insert_activity("2026-09-22", 200), std::exception);
}

TEST_F(HealthSchemaTest, SyncRunLifecycle) {
    Repositories::SyncRunRepository runs;
    const long id = runs.create("2026-09-01", "2026-09-07", {"steps", "sleep"});
    runs.finish(id, "succeeded", nlohmann::json{{"steps", {{"added", 3}}}});
    const auto row = runs.get(id);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ((*row)["status"], "succeeded");
    EXPECT_EQ((*row)["result"]["steps"]["added"], 3);
}
```

Хелперы `count_rows`/`insert_activity` в фикстуре через `Database::get()`, как в тестах плана 1.

- [ ] **Step 3: Красный прогон CI** — падение на отсутствии заголовков.
- [ ] **Step 4: Реализация** — миграция, структуры с `to_json`, репозиторий журнала.
- [ ] **Step 5: Зелёный прогон CI, слияние.**

---

### Task 2: Нормализация daily_activity

**Files:**
- Create: `src/xiaomi/Normalize.hpp`, `src/xiaomi/Normalize.cpp`
- Create: `src/repositories/ActivityRepository.hpp`
- Create: `tests/unit/test_normalize_activity.cpp`, `tests/integration/test_activity_repository.cpp`
- Create: `tests/fixtures/xiaomi_raw_samples.json` (синтетика в форме сырых записей облака)

**Interfaces:**
- Consumes: `Domain::DailyActivity`
- Produces:
  - `struct Xiaomi::ActivityResult { std::vector<Domain::DailyActivity> days; long suppressed_steps; long skipped; }`
  - `ActivityResult Xiaomi::normalize_daily_activity(const std::vector<nlohmann::json>& step_records, const std::vector<nlohmann::json>& calorie_records, std::string_view user_id)`
  - `struct Repositories::UpsertCounts { long added; long updated; }`
  - `UpsertCounts ActivityRepository::upsert(const std::vector<Domain::DailyActivity>&)`

Семантика эталона дословно (`iter_daily_activity`): группировка по (локальная дата из `time` с `zone_offset`, локальная минута `HH:MM`); при коллизии минуты остаётся запись с большим кортежем (steps, distance, calories), подавленные шаги суммируются в `suppressed_steps`; суточные значения складываются из минут; калории берутся ОТДЕЛЬНЫМ ключом `calories` и ЗАМЕНЯЮТ суммированные из шагов по датам, где есть; timezone дня это `zone_name` последней записи; `collected_at` дня это максимум по записям; записи с битым `time` или до 2000 года в skipped.

- [ ] **Step 1: Падающие юнит-тесты** — минимум шесть: коллизия минут двух устройств (максимум, не сумма; suppressed считается); детерминизм при равных кортежах; калории заменяют, а не добавляются; запись до 2000 года в skipped; пустой вход даёт пусто; сутки считаются в поясе записи (`zone_offset`), а не UTC.
- [ ] **Step 2: Красный CI.**
- [ ] **Step 3: Реализация + репозиторий.** Upsert:

```sql
INSERT INTO daily_activity (user_id, device_id, date, steps, distance_m, active_kcal, timezone, collected_at, updated_at)
VALUES ($1, '', $2, $3, $4, $5, $6, $7, now())
ON CONFLICT (user_id, date, device_id) DO UPDATE SET
  steps = EXCLUDED.steps, distance_m = EXCLUDED.distance_m,
  active_kcal = EXCLUDED.active_kcal, timezone = EXCLUDED.timezone,
  collected_at = EXCLUDED.collected_at, updated_at = now()
RETURNING (xmax = 0) AS inserted
```

- [ ] **Step 4: Интеграционные тесты идемпотентности** — два одинаковых upsert: первый added, второй updated, count строк не растёт.
- [ ] **Step 5: Зелёный CI, слияние.**

---

### Task 3: Нормализация сна и суточная оценка

**Files:**
- Modify: `src/xiaomi/Normalize.hpp/.cpp`, `src/xiaomi/CloudClient.hpp/.cpp`
- Create: `src/repositories/SleepRepository.hpp`
- Create: `tests/unit/test_normalize_sleep.cpp`, `tests/integration/test_sleep_repository.cpp`

**Interfaces:**
- Produces:
  - `std::vector<nlohmann::json> CloudClient::fetch_daily_sleep_reports(start_date, end_date)` — свой эндпоинт и строгие правила курсора из Global Constraint 3
  - `std::vector<Domain::SleepSession> Xiaomi::normalize_sleep(const std::vector<nlohmann::json>& records, std::string_view user_id, long& skipped)`
  - `void Xiaomi::apply_daily_sleep_scores(std::vector<Domain::SleepSession>&, const std::vector<nlohmann::json>& reports)`
  - `UpsertCounts SleepRepository::upsert(...)` с `sleep_score = COALESCE(EXCLUDED.sleep_score, sleep_sessions.sleep_score)` и тем же для источника

Семантика эталона дословно: начало из `bedtime | device_bedtime | bed_timestamp`, конец из `wake_up_time | device_wake_up_time | out_bed_timestamp | time`; стадии из `items[]` по маппингу 2 deep, 3 light, 4 rem, 5 awake, неизвестное это light; `sleep_id = "<sid|user_id>_<time|end>"`; оценка валидна только целая в (0, 100], нули и дробные это «нет»; кандидат на суточную оценку: не nap, длительность в (0, 1440] минут и (0, 24] часов по границам; отчёт применяется только при совпадении даты пробуждения, источника (sid отчёта или did из payload, "default" и пустое это «любой») и, при наличии `segment_details`, ровно одного самого длинного сегмента с точным совпадением границ; несколько предложений одной сессии с разными баллами это «нет оценки»; провал запроса отчётов не трогает сессии.

- [ ] **Step 1: Падающие юнит-тесты** — минимум восемь, включая: неоднозначные кандидаты не получают score; сегментные границы решают; конфликт баллов даёт NULL; nap не получает суточный score; невалидный сегмент отчёта отбрасывает отчёт, не сессии; fetch_daily_sleep_reports на подделке отвергает нестроковый и повторный курсор.
- [ ] **Step 2: Красный CI.** **Step 3: Реализация.**
- [ ] **Step 4: Интеграционный тест COALESCE** — upsert сессии со score, затем той же сессии с NULL score: score в базе остаётся; затем с новым score: заменяется.
- [ ] **Step 5: Зелёный CI, слияние.**

---

### Task 4: Остальные пять типов

**Files:**
- Modify: `src/xiaomi/Normalize.hpp/.cpp`, `src/xiaomi/CloudClient.hpp/.cpp` (`fetch_sport_records`)
- Create: `src/repositories/{Workout,Body,Samples}Repository.hpp`
- Create: `tests/unit/test_normalize_rest.cpp`, `tests/integration/test_rest_repositories.cpp`

Семантика эталона дословно: тренировки со своего эндпоинта, `end_time` при отсутствии это `start_time + duration`, `activity_type` из `category | key | sport_type | "workout"`, все метрики через optional (ноль это NULL); вес: запись без веса пропускается целиком, остальные поля optional; пульс: `type == 0` passive, иначе active, resting отдельным ключом с `sample_type = "resting"` и временем из `date_time | time`; spo2 из `spo2 | value`; стресс из `stress | score | value`, уровень `<30 low, <60 medium, иначе high`.

- [ ] Шаги по той же схеме: падающие тесты (по два-три на тип, обязательно ноль в NULL и пропуск записи без веса), красный CI, реализация, идемпотентность в интеграции, зелёный CI, слияние.

---

### Task 5: SyncService и журнал

**Files:**
- Create: `src/sync/SyncService.hpp`, `src/sync/SyncService.cpp`
- Create: `tests/unit/test_sync_service.cpp` (на подделке транспорта), `tests/integration/test_sync_full.cpp`
- Modify: `docs/module-deps.txt` (`sync -> xiaomi, repositories, database, utils`)

**Interfaces:**
- Produces: `nlohmann::json Sync::SyncService::run(from, to, data_types)` — на каждый тип: куски по `xiaomi.sync_chunk_days` (конфиг, по умолчанию 7), потолок времени на тип `xiaomi.sync_type_timeout_seconds` (по умолчанию 180), счётчики added/updated/skipped/suppressed_steps, продолжение после провала типа с записью класса ошибки (`auth` / `protocol` / `other`), обновление `sync_state`; advisory lock `pg_try_advisory_lock(kSyncAdvisoryLockKey)`, занято — статус `skipped`.

- [ ] Падающие тесты: два прохода на одних фикстурах дают added потом updated (Review Focus 1); провал sleep не мешает weight (Review Focus 5); auth-ошибка помечает тип `auth` и не ретраится внутри; занятый lock даёт skipped. Красный CI, реализация, зелёный, слияние. Новые конфиг-ключи через гейт синхронизации.

---

### Task 6: Воркер, маршруты синка, деплой воркера

**Files:**
- Create: `src/jobs/XiaomiSyncHandler.hpp` (регистрация в `register_builtin_handlers`)
- Create: `src/api/SyncController.hpp` (через `new-endpoint.sh`, POST /api/v1/sync и GET /api/v1/sync/{id})
- Modify: `docs/openapi.yaml` + перегенерация артефактов
- Modify: `deploy/values-prod.yaml` (воркер: extraEnvFrom с mi-fitness-app, образ 1.8.x)

**Interfaces:**
- Produces: обработчик `"xiaomi_sync"` payload `{run_id, from, to, data_types}` вызывает SyncService и пишет результат в `sync_runs`; `POST /api/v1/sync` валидирует диапазон, создаёт run, кладёт задание, отвечает 202 с `run_id`; `GET /api/v1/sync/{id}` отдаёт журнал.

- [ ] Красные тесты api-бакета на маршруты и юнит на регистрацию обработчика; реализация; релиз 1.8.0; деплой чарта воркера `helm/mi-fitness-api-worker` в namespace `mi-fitness-api` с теми же значениями базы и Redis; живой запуск `POST /api/v1/sync` малого диапазона и чтение `GET /sync/{id}`.

---

### Task 7: Живая сверка с Python-мостом

**Files:**
- Create: `tools/compare_with_python_bridge.py`
- Create: `docs/parity-report-2026-09.md`

- [ ] **Step 1:** Полный синк июль-сентябрь через `POST /api/v1/sync` с поднятым `MI_FITNESS_SYNC_TYPE_TIMEOUT`.
- [ ] **Step 2:** Достать SQLite Python-моста с PVC (одноразовый под с volume, файл наружу через kubectl cp), НЕ поднимая его процессов.
- [ ] **Step 3:** Скрипт сравнивает по дням: шаги, дистанция, ккал; по сессиям сна: границы, длительность, score, provenance; вес по замерам; счётчики остальных типов. Допуск нулевой, каждое расхождение либо объяснено (пересинк задним числом, известное поведение), либо это дефект и чинится до закрытия плана.
- [ ] **Step 4:** Отчёт в docs, вывод в журнал. Удаление Python-моста сюда НЕ входит, это план 3.

## Чего в плане нет

Маршруты чтения данных, экспорт, расписание `Tasks::`, объединение сессии probe и синка, вывод Python-моста из эксплуатации — план 3.
