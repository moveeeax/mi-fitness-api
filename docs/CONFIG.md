# Configuration reference

Every knob has three ways in, tried in order:

1. **Environment variable** (highest priority — for containers).
2. **`config/config.json`** value, with `${VAR}` / `${VAR:-default}` expansion.
3. **Built-in default** baked into the code.

Set `CONFIG_FILE` to point at a different JSON file (e.g.
`config/worker.json` for the worker binary).

---

## App

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `APP_ENV` | `app.env` | string | `development` | Environment label used by boot-time config validation. `production` / `prod` makes `AUTH_MODE=none` a fatal boot error and warns on insecure combinations (cookie `secure=false`, rate limit off / fail-open, docs on, cookie auth without CSRF) |

## Server

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `SERVER_HOST` | `server.host` | string | `0.0.0.0` | Listen address |
| `SERVER_PORT` | `server.port` | int | `8080` | |
| `SERVER_THREADS` | `server.threads` | int | `0` (auto = #cores) | Drogon event-loop threads. Under the **synchronous** pqxx model the in-flight DB-call count is capped by THIS, not by `database.pool_size` — it's the real concurrency knob. `0`/unset auto-sizes to the CPU count; keep `database.pool_size` ≥ threads (the app warns at boot if not). |
| `SERVER_MAX_BODY_BYTES` | `server.max_body_bytes` | int | `10485760` | 10 MB cap on request bodies — prevents memory blow-up from a single client. Bump for file uploads. |
| `SERVER_SSL_ENABLED` | `server.ssl.enabled` | bool | `false` | Off by default — production terminates TLS at the ingress/reverse proxy (the Helm chart assumes this). Exposing the app directly (bare-metal, no proxy)? set `true` + cert/key, else traffic is plain HTTP. |
| `SSL_CERT_FILE` | `server.ssl.cert` | string | — | PEM cert path when SSL on |
| `SSL_KEY_FILE` | `server.ssl.key` | string | — | PEM key path when SSL on |
| `SHUTDOWN_PRE_STOP_DELAY_SEC` | `shutdown.pre_stop_delay_sec` | int | `5` | Seconds between "readiness = 503" and Drogon quit |

## API & middleware

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `API_PUBLIC_PATHS` | `api.public_paths` | csv | `/`, the probes (`/healthz,/ready,/health,/metrics`), `/api/v1/docs`, `/api/v1/openapi.yaml`, `/api/v1/auth/{login,register,refresh}`, `/api/v1/account/confirm/*`, `/api/v1/account/reset-password-request`, `/api/v1/account/reset-password/*`, `/api/v1/account/change-email/*`, `/api/v1/account/join-from-invite/*` (`Utils::Strings::kDefaultPublicPathsCsv`) | Paths that bypass auth. Exact-match; a trailing `*` is a prefix match (used for the token-bearing account routes). FULL OVERRIDE of the built-in default — prefer the extra key below for additions. |
| `API_PUBLIC_PATHS_EXTRA` | `api.public_paths_extra` | csv | — | ADDITIVE companion to `API_PUBLIC_PATHS`: entries are appended to the resolved public-paths set (built-in default or override), same matching rules. Use it to open extra routes (a payment-provider webhook, a public feed) without re-listing — and risking silently dropping — the default set. |
| `CORS_ALLOWED_ORIGINS` | `cors.allowed_origins` | csv | — | Empty disables CORS |

## Auth

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `AUTH_MODE` | `auth.mode` | enum | `none` | `none` \| `bearer` \| `jwt` |
| `AUTH_BEARER_TOKEN` | `auth.bearer_token` | string | — | Required when `mode=bearer` |
| `JWT_SECRET` | `auth.jwt.secret` | string | — | Required when `mode=jwt` |
| `JWT_ISSUER` | `auth.jwt.issuer` | string | — | Checked if non-empty |
| `JWT_AUDIENCE` | `auth.jwt.audience` | string | — | Checked if non-empty |
| `JWT_LEEWAY_SEC` | `auth.jwt.leeway_sec` | int | `30` | Clock skew tolerance |
| `JWT_ROLES_CLAIM` | `auth.jwt.roles_claim` | string | `roles` | JSON claim for RBAC |
| `JWT_SCOPES_CLAIM` | `auth.jwt.scopes_claim` | string | `scope` | Space-separated per OAuth2 |
| `AUTH_COOKIES_ENABLED` | `auth.cookies.enabled` | bool | `false` | Cookie sessions for the SPA (access+refresh) |
| `AUTH_COOKIE_ACCESS` | `auth.cookies.access_name` | string | `__Host-access` | Strip `__Host-` prefix for plain-http dev |
| `AUTH_COOKIE_REFRESH` | `auth.cookies.refresh_name` | string | `__Host-refresh` | |
| `AUTH_COOKIE_ACCESS_TTL_SEC` | `auth.cookies.access_ttl_sec` | int | `900` | 15 min |
| `AUTH_COOKIE_REFRESH_TTL_SEC` | `auth.cookies.refresh_ttl_sec` | int | `604800` | 7 days |
| `AUTH_COOKIE_SECURE` | `auth.cookies.secure` | bool | `true` | Set `false` only for http://localhost |
| `AUTH_COOKIE_SAMESITE` | `auth.cookies.samesite` | enum | `Lax` | `Lax` \| `Strict` \| `None` |
| `AUTH_COOKIE_REVOCATION_PREFIX` | `auth.cookies.refresh_revocation_prefix` | string | `auth:refresh:` | Redis prefix for refresh-JTI revocation |

## Rate limit

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `RATE_LIMIT_ENABLED` | `rate_limit.enabled` | bool | `false` | |
| `RATE_LIMIT_REQUESTS` | `rate_limit.requests` | int | `60` | Max per window |
| `RATE_LIMIT_WINDOW_SEC` | `rate_limit.window_sec` | int | `60` | |
| `RATE_LIMIT_SCOPE` | `rate_limit.scope` | enum | `ip_or_user` | `ip` \| `ip_or_user` |
| `RATE_LIMIT_TRUST_PROXY` | `rate_limit.trust_proxy` | bool | `false` | Use `X-Forwarded-For` |
| `RATE_LIMIT_TRUSTED_PROXY_COUNT` | `rate_limit.trusted_proxy_count` | int | `1` | With `trust_proxy` on: number of trusted hops appended to XFF (indexed from the right); values < 1 are clamped to 1 |
| `RATE_LIMIT_FAIL_OPEN` | `rate_limit.fail_open` | bool | `true` | Allow if Redis is down |
| `RATE_LIMIT_WHITELIST` | `rate_limit.whitelist` | csv | — | IPs / user IDs that bypass |
| `RATE_LIMIT_PROTECTED_REQUESTS` | `rate_limit.protected_requests` | int | `10` | Stricter budget for the auth-public brute-force surfaces (login/register/refresh, token-bearing account links, public content — `Utils::Strings::kDefaultProtectedPathsCsv`), which the general limiter skips as public paths |
| `RATE_LIMIT_PROTECTED_WINDOW_SEC` | `rate_limit.protected_window_sec` | int | `60` | Window for the protected-path budget |
| `RATE_LIMIT_PROTECTED_PATHS` | `rate_limit.protected_paths` | csv | `Utils::Strings::kDefaultProtectedPathsCsv` | The protected-budget path set (same matching rules as public paths). FULL override of the built-in default — env or code default only; deliberately NOT in `config/*.json` (a present-but-empty string value would silently empty the set, see `docs/config-sync-allowlist.txt`) |

## Security headers & CSRF

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `SECURITY_CSRF_ENABLED` | `security.csrf.enabled` | bool | `false` | Double-submit CSRF check for cookie-auth mutations; production config validation warns if cookie auth is on without it |
| `SECURITY_CSRF_COOKIE` | `security.csrf.cookie_name` | string | `csrf-token` | Readable (non-HttpOnly) cookie the SPA mirrors |
| `SECURITY_CSRF_HEADER` | `security.csrf.header_name` | string | `X-CSRF-Token` | Header the mirrored token must arrive in |
| `SECURITY_HSTS` | `security.hsts` | bool | `false` | Emit `Strict-Transport-Security` (only makes sense behind TLS) |
| `SECURITY_HSTS_MAX_AGE` | `security.hsts_max_age` | int | `31536000` | `max-age` in seconds (1 year) |

## Idempotency

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `IDEMPOTENCY_ENABLED` | `idempotency.enabled` | bool | `false` | |
| `IDEMPOTENCY_TTL_SEC` | `idempotency.ttl_sec` | int | `86400` | |
| `IDEMPOTENCY_MAX_BODY_KB` | `idempotency.max_body_kb` | int | `1024` | Reject oversized request bodies (413) |
| `IDEMPOTENCY_MAX_RESPONSE_KB` | `idempotency.max_response_kb` | int | `256` | Skip caching oversized responses (no replay) |
| `IDEMPOTENCY_LOCK_TTL_SEC` | `idempotency.lock_ttl_sec` | int | `30` | In-flight lock for concurrent same-key requests |

## Docs / Swagger UI

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `DOCS_ENABLED` | `docs.enabled` | bool | `false` | Mount `/api/v1/docs` + `/api/v1/openapi.yaml` — dev only |
| `DOCS_OPENAPI_PATH` | `docs.openapi_path` | string | `docs/openapi.yaml` | Path served at `/api/v1/openapi.yaml` |

## Observability

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `LOG_NAME` | `logging.name` | string | `mi_fitness_api` | |
| `LOG_FILE` | `logging.file` | string | `logs/app.log` | |
| `LOG_LEVEL` | `logging.level` | enum | `info` | trace/debug/info/warn/error/critical |
| `LOG_FORMAT` | `logging.format` | enum | `text` | `text` (human) or `json` (one JSON object per line for Loki/ELK) |
| `METRICS_ADDRESS` | `observability.metrics_address` | string | `0.0.0.0:9090` | |
| `SERVICE_NAME` | `observability.service_name` | string | `mi_fitness_api_service` | Also emitted as `service` field in JSON logs |
| `OTLP_ENDPOINT` | `observability.otlp_endpoint` | string | — | OTLP HTTP traces endpoint. Empty + `trace_stdout=false` → no-op tracer |
| `TRACE_STDOUT` | `observability.trace_stdout` | bool | `false` | Synchronous stdout span exporter for debugging. When `OTLP_ENDPOINT` is empty and this is off, tracing is a no-op |

## Database

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `DATABASE_PRIMARY_URL` | `database.primary` | string | `postgresql://localhost:5432/appdb` | Connection string |
| `DATABASE_REPLICA_URLS` | `database.replicas` | csv | — | Read replicas (full URLs) |
| `DATABASE_REPLICA_HOSTS` | — | csv | — | Replica HOSTS only (env-only; checked when `DATABASE_REPLICA_URLS` is unset): each host is assembled into a DSN with the primary's `DATABASE_PORT`/`DATABASE_USER`/`DATABASE_NAME`/`DATABASE_PASSWORD`, so the password is never baked into a URL env var |
| `DB_POOL_SIZE` | `database.pool_size` | int | `10` | Per-pool connections (primary + each replica). Keep ≥ `server.threads`: a smaller pool makes IO threads queue on `acquire()`; a much larger pool leaves the extra connections inert (and the `db_pool` saturation gauge under-reports). |
| `DB_ACQUIRE_TIMEOUT_MS` | `database.acquire_timeout_ms` | int | `5000` | |
| `DB_STATEMENT_TIMEOUT_MS` | `database.statement_timeout_ms` | int | `30000` | Per-connection PostgreSQL `statement_timeout`. `0` disables. |
| `DB_MIGRATIONS_ENABLED` | `database.migrations_enabled` | bool | `true` | Set `false` when init-container runs them |
| `DB_MIGRATIONS_DIR` | `database.migrations_dir` | string | `migrations` | |
| `DB_RETRY_MAX_ATTEMPTS` | `database.retry.max_attempts` | int | `2` | |
| `DB_RETRY_BASE_DELAY_MS` | `database.retry.base_delay_ms` | int | `20` | |
| `DB_RETRY_MAX_DELAY_MS` | `database.retry.max_delay_ms` | int | `200` | |
| `DB_RETRY_JITTER` | `database.retry.jitter` | bool | `true` | Full-jitter backoff |
| `DB_POOL_METRIC_REFRESH_SEC` | `database.pool_metric_refresh_sec` | int | `10` | Refresh interval for the `db_pool_active_connections` / `db_pool_size` gauges |

For individual Postgres URL components used by the sample config:
`DATABASE_USER`, `DATABASE_PASSWORD`, `DATABASE_HOST`, `DATABASE_PORT`, `DATABASE_NAME`.

`DATABASE_REQUIRE_SECURE_PASSWORD` (env-only flag): an empty or known-weak DB
password normally logs a warning at boot; set this to `true` to make it fatal.

### Read replicas and `DB_POOL_SIZE`

Setting `DATABASE_REPLICA_URLS` routes most reads to a replica, but a few
paths deliberately read from the **primary** to get read-after-write
consistency (via `Database::execute_read_primary`), regardless of the replica
config: the account email worker, the admin "update-echo" read-back after a
mutation, and `--verify-migrations`. So sizing `DB_POOL_SIZE` (the primary
pool) only for HTTP traffic under-counts: the background email worker competes
for the same primary connections. Budget primary `DB_POOL_SIZE` for request
handlers **plus** the worker, even when replicas absorb the bulk of reads.

## Cache (Redis)

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `REDIS_URL` | `cache.url` | string | `tcp://127.0.0.1:6379` | Standalone mode |
| `REDIS_PASSWORD` | `cache.password` | string | — | |
| `REDIS_DB` | `cache.db` | int | `0` | Logical Redis DB index (`SELECT n`, via `ConnectionOptions.db`). **Set this whenever the Redis instance is shared with another application** — job-queue (`jobs:queue:*`), rate-limit, idempotency and refresh-token-revocation keys aren't prefixed per app, so two apps on the same DB collide. Applies to standalone AND Sentinel connections, and to the worker's separate blocking-BRPOP client — API and worker must use the same value. `parse_redis_url()` does NOT read a trailing `/N` from `REDIS_URL` — use `REDIS_DB`, not the URL. |
| `CACHE_POOL_SIZE` | `cache.pool_size` | int | `10` | |
| `REDIS_USE_SENTINEL` | `cache.use_sentinel` | bool | `false` | |
| `REDIS_MASTER_NAME` | `cache.sentinel.master_name` | string | `mymaster` | |
| `REDIS_SENTINEL_NODES` | `cache.sentinel.nodes` | csv | `localhost:26379` | Env: `host:port,host:port,...`. The JSON key is an array of `{host, port}` objects, not a csv string |
| `REDIS_SENTINEL_PASSWORD` | `cache.sentinel.password` | string | falls back to `REDIS_PASSWORD` | |
| `REDIS_SOCKET_TIMEOUT_MS` | `cache.socket_timeout_ms` | int | `500` | Per-command timeout; tighten under low-latency hot paths, loosen for large values / EVAL. |
| `REDIS_POOL_WAIT_TIMEOUT_MS` | `cache.pool_wait_timeout_ms` | int | `500` | Max wait for a free connection from the pool. |

For URL components: `REDIS_HOST`, `REDIS_PORT`.

## Messaging (Kafka)

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|

## Jobs

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `JOBS_ENABLED` | `jobs.enabled` | bool | `false` | |
| `JOBS_RESULT_TTL` | `jobs.result_ttl` | int | `86400` | |
| `JOBS_MAX_RETRIES` | `jobs.max_retries` | int | `3` | |
| `JOBS_RETRY_BACKOFF_BASE_MS` | `jobs.retry_backoff_base_ms` | int | `0` | Delay before a failed job is retried (exponential, capped by the max below). `0` keeps the legacy immediate requeue |
| `JOBS_RETRY_BACKOFF_MAX_MS` | `jobs.retry_backoff_max_ms` | int | `60000` | Cap on the retry backoff |
| `JOBS_VISIBILITY_TIMEOUT_SEC` | `jobs.visibility_timeout_sec` | int | `0` | Processing lease: a job whose worker dies is re-queued after this many seconds. `0` disables leases (legacy behaviour) |
| `JOBS_DLQ_METRIC_REFRESH_SEC` | `jobs.dlq_metric_refresh_sec` | int | `10` | Exports `jobs_dlq_depth{type="..."}` plus an aggregate `type="_total"` |
| `JOBS_QUEUE_METRIC_REFRESH_SEC` | `jobs.queue_metric_refresh_sec` | int | `10` | Same bookkeeping for the waiting queue: `jobs_queue_depth{type="..."}` plus `type="_total"` |
| `DB_REPLICA_LAG_METRIC_REFRESH_SEC` | `database.replica_lag_metric_refresh_sec` | int | `15` | Refresh interval for the `db_replica_lag_seconds` gauge. Only registered when read replicas are configured (primary has no replay timestamp). |

## Billing module

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| — | `billing.provider` | string | `paypal` | Only provider supported today |

The webhook path (``) is auth-public in the
shipped defaults (PayPal's own servers call it directly, not an
authenticated user). A deployment that overrides `api.public_paths` must
re-expose it through the ADDITIVE `api.public_paths_extra` /
`API_PUBLIC_PATHS_EXTRA` — never by re-listing the full override (see the
warning above).

Billing's transactional emails (top-up receipt, refund/reversal notice,
failed-payment notice, and the admin adjust `notify` flag's adjustment
notice) have no config keys of their own: they ride the generic
`email.send` job type and the Mail settings below (`mail.enabled`,
`mail.via_jobs`, SMTP block). Delivery is best-effort by contract — a mail
outage can never affect the money path.

## Mail (SMTP)

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|

## Worker (second binary, `mi_fitness_api_worker`)

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `WORKER_ID` | `worker.id` | string | `worker-1` | |
| `WORKER_TYPES` | `worker.types` | csv | all registered handler types | Queues the worker pulls from. Unset everywhere → every type with a registered handler. When you DO set it (the compose stack ships `default,account_email`; `config.json` ships `default`), it MUST include `account_email`, `email.send`, and `webhook.deliver` or those jobs pile up undrained. |
| `WORKER_STRICT_TYPES` | `worker.strict_types` | bool | `false` | A `WORKER_TYPES` entry without a registered handler logs a warning; `true` upgrades it to a refusal to start |
| `WORKER_CONCURRENCY` | `worker.concurrency` | int | `2` | |
| `WORKER_HEALTH_PORT` | `worker.health_port` | int | `9091` | |
| `WORKER_BRPOP_TIMEOUT` | `worker.brpop_timeout` | int | `5` | |

## Conventions

- `csv`: comma-separated values (whitespace around commas is trimmed). Empty components dropped.
- `bool`: truthy values are exactly `true`, `1`, `yes` (`Utils::Strings::flag_true`);
  anything else is false. The same rule applies to standalone env flags like
  `RUN_MIGRATIONS_ONLY` and `DATABASE_REQUIRE_SECURE_PASSWORD`.
- `enum`: invalid values fall back to the default, never throw.
- Passwords and secrets must never be committed to `config/*.json` — use `${VAR}`
  placeholders so the checked-in file stays safe.
- Config files live under `/app/config` in the Docker image; mount a volume or
  set `CONFIG_FILE` to point at something else.

## Local override pattern

```bash
# config/local.json (gitignored)
{
  "server": { "port": 8081 },
  "auth":   { "mode": "jwt" }
}

CONFIG_FILE=config/local.json ./mi_fitness_api
```

## Xiaomi

| Env | JSON key | Type | Default | Notes |
|---|---|---|---|---|
| `MI_FITNESS_TOKEN_KEY` | `xiaomi.token_key` | string | `""` | Base64 of the 32-byte libsodium secretbox key that seals the rotated Mi Fitness passToken in Postgres. Empty means the Xiaomi module is not configured; the probe endpoint answers 503 `not_configured`. Never log or commit this value |
| `MI_FITNESS_USER_ID` | `xiaomi.user_id` | string | `""` | Numeric Xiaomi account id, seed only |
| `MI_FITNESS_PASS_TOKEN` | `xiaomi.pass_token` | string | `""` | Seed passToken from the cluster Secret. The rotated token in Postgres always wins; this value is only written when the table is empty or reseed is set. Never log or commit |
| `MI_FITNESS_REGION` | `xiaomi.region` | string | `cn` | Cloud region candidate (ru, cn, de, i2, sg, us) |
| `MI_FITNESS_RESEED` | `xiaomi.reseed` | bool | `false` | Emergency lever: overwrite the stored token with the seed on next boot. Turn off afterwards |
| `MI_FITNESS_CHUNK_DAYS` | `xiaomi.sync_chunk_days` | int | `7` | Width of one sync window in days |
| `MI_FITNESS_SYNC_TYPE_TIMEOUT` | `xiaomi.sync_type_timeout_seconds` | int | `180` | Wall-clock budget per data type, checked between chunks. Raise for deep backfills. daily_activity aggregates over the whole range and writes once at the end, so a budget overrun for that type stores nothing for the run |
| `MI_FITNESS_HTTP_TIMEOUT` | `xiaomi.http_timeout_seconds` | int | `20` | libcurl budget for ONE cloud request. Deep backfill responses take longer than the default |
