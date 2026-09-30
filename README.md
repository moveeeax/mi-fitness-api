# mi-fitness-api

REST-сервис синхронизации данных Mi Fitness (Xiaomi Cloud) в Postgres.
Порт Python-моста [mi_fitness_data_bridge](https://github.com/shkyyy18/mi_fitness_data_bridge)
на C++20 поверх шаблона cpp-rapid-rest-template: Drogon, PostgreSQL, Redis.

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Drogon](https://img.shields.io/badge/Drogon-HTTP%20Framework-green.svg)
![PostgreSQL](https://img.shields.io/badge/PostgreSQL-15-336791.svg)
![Redis](https://img.shields.io/badge/Redis-7-DC382D.svg)
![License](https://img.shields.io/badge/License-AGPL--3.0--only-blue.svg)

## Что делает

Сервис ходит в закрытый протокол облака Mi Fitness (serviceLogin с passToken,
RC4-шифрование тел, подписи SHA1, пагинация по курсору), нормализует восемь
типов данных здоровья и складывает их в Postgres с естественными ключами
уникальности:

- суточная активность (шаги, дистанция, калории),
- сон с сессиями, стадиями и суточной оценкой,
- пульс, стресс, SpO2 (посэмплово),
- тренировки, замеры тела, события аномального пульса.

Паритет с Python-мостом проверен построчной сверкой на трёх месяцах живых
данных: [docs/parity-report-2026-09.md](docs/parity-report-2026-09.md).

## API

| Маршрут | Что делает |
|---|---|
| `GET /api/v1/xiaomi/probe` | Живая проверка учётных данных и связи с облаком |
| `POST /api/v1/sync` | Поставить синк диапазона дат в очередь (исполняет воркер) |
| `GET /api/v1/sync/{id}` | Журнал запуска: статус и счётчики по типам |

Плюс служебные: аутентификация по JWT-кукам (`/api/v1/auth/*`), админ
пользователей и ролей, API-ключи, Jobs API с DLQ, `/healthz`, `/metrics`.
Полная спека: [docs/openapi.yaml](docs/openapi.yaml), Swagger UI на
`/api/v1/docs`.

Регистрации нет: пользователей заводит админ (`POST /api/v1/admin/users`).

## Учётные данные Xiaomi

Токен (`passToken`) живёт в Kubernetes Secret, при каждом логине облако его
ротирует, свежая версия шифруется libsodium secretbox и сохраняется в
Postgres. Значение токена не появляется ни в логах, ни в текстах ошибок.
Ключи конфигурации: `MI_FITNESS_USER_ID`, `MI_FITNESS_PASS_TOKEN`,
`MI_FITNESS_TOKEN_KEY`, `MI_FITNESS_REGION` (см. [docs/CONFIG.md](docs/CONFIG.md)).

## Сборка и тесты

Локальная компиляция в этом репозитории запрещена (см. [CLAUDE.md](CLAUDE.md)):
сборку и тесты гоняет только GitHub Actions — юнит, интеграционные и API-тесты
против настоящих Postgres/Redis, e2e с реальным HTTP-сервером, ASan/UBSan/TSan,
clang-tidy, gitleaks и набор гейтов синхронизации (маршруты ↔ реестр ↔ OpenAPI,
конфиг ↔ доки ↔ helm, версии ↔ changelog).

Локально живут только лёгкие проверки: `make fmt`, `scripts/check-*.sh`,
рендер Helm.

## Деплой

Два чарта: `helm/mi-fitness-api` (API) и `helm/mi-fitness-api-worker`
(воркер очереди `xiaomi_sync`). Боевые значения в `deploy/`:

```bash
helm upgrade mi-fitness-api helm/mi-fitness-api \
  -n mi-fitness-api -f deploy/values-prod.yaml
helm upgrade mi-fitness-api-worker helm/mi-fitness-api-worker \
  -n mi-fitness-api -f deploy/values-worker-prod.yaml
```

База — CNPG-кластер (манифест в `deploy/db/`), данные без PVC у подов.
Ручки таймаутов глубокого бэкфила: `deploy/sync-tuning-configmap.yaml`.
Релиз: `./scripts/release.sh <ver>` + тег `v<ver>` — пайплайн собирает и
подписывает образы (cosign, SBOM).

## Документация

[docs/INDEX.md](docs/INDEX.md) — карта всей документации,
[docs/CONVENTIONS.md](docs/CONVENTIONS.md) — паттерны кода,
[docs/CONFIG.md](docs/CONFIG.md) — все ключи конфигурации.

## Лицензия

AGPL-3.0-only, как у исходного моста.
