# Changelog

All notable changes to this project are documented here.
Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning: [SemVer](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.8.0] — 2026-09-30

### Added
- Синк данных Mi Fitness в Postgres: нормализация восьми типов по правилам эталона, журнал запусков sync_runs, воркер с обработчиком xiaomi_sync, маршруты POST /api/v1/sync и GET /api/v1/sync/{id}

### Fixed
- Временный каталог загрузок drogon увезён в /tmp: дефолтный ./uploads упирался в read-only корень контейнера и сыпал 256 строк ошибок на каждый старт

## [1.7.3] — 2026-09-30

### Fixed
- Классификация отказов облака выровнена с Python-эталоном: 5xx логина это ошибка протокола, а не мёртвые учётные данные, has_more принимается числом, пустой next_key завершает пагинацию; user_id и region проверяются при записи и чтении учётных данных; чарт воркера получил extraEnvFrom

## [1.7.2] — 2026-09-30

### Fixed
- Каждый запрос к облаку Xiaomi несёт User-Agent: без него облако отвечает конвертом с passToken:null, и логин падает на отсутствии поля

## [1.7.1] — 2026-09-29

### Fixed
- Отказ облака Xiaomi в маршруте probe теперь пишется в лог сервиса с текстом исключения: без этого не отличить мёртвый токен от сломанной ступени логина

## [1.7.0] — 2026-09-29

### Added
- Порт моста Mi Fitness на C++: крипта закрытого протокола, сверенная с Python-реализацией по золотым векторам, двухступенчатый логин с ротацией passToken, подписанные запросы с курсорной пагинацией, хранение токена в Postgres под libsodium с посевом из Secret при старте, маршрут GET /api/v1/xiaomi/probe и манифесты деплоя без PVC

## [1.6.0] — 2026-08-23

### Changed
- `app_core` is now a STATIC library (was INTERFACE) and the billing module
  is de-inlined into it — non-template bodies of `billing/Wallet`,
  `billing/PayPalClient`, `api/BillingController`,
  `api/AdminBillingController` and `email/BillingEmails` moved to paired
  `.cpp` files compiled ONCE instead of once per binary (Phase 2 of the
  modularity design, ADR 0003 amended). Headers slimmed accordingly:
  `PayPalClient.hpp` no longer drags `curl.h`/json/spdlog into every
  including TU. CMake GLOBs `src/**/*.cpp` into `app_core`, so future
  module `.cpp` files link with no CMake edit; the PCH is owned by
  `app_core` and reused by the app/worker binaries. No behavior change.
- The jobs module is de-inlined the same way — non-template bodies of
  `jobs/Jobs` (the `JobQueue` manager), `jobs/Dispatcher` and
  `jobs/BuiltinHandlers` moved to paired `.cpp` files in `app_core`
  (`jobs/Job.hpp` stays header-only: domain struct + key helpers per
  ADR 0003). Jobs headers no longer drag redis++/spdlog/Cache/Trace or the
  email/webhooks handler headers into including TUs, and
  `email/Mailer.hpp` no longer includes `jobs/Jobs.hpp`
  (`detail::via_jobs()` body moved to `email/Mailer.cpp`) — the
  email↔jobs include cycle is now broken at the header level. No behavior
  change.
- The composition root and the middleware chain are de-inlined the same way
  — non-template bodies of `core/Core` (`Core::Application`
  initialize/shutdown orchestration, metric registrars, health registry,
  singleton) and `api/Middleware` (the whole advice chain, access log, HTTP
  metrics, docs endpoints) moved to paired `.cpp` files in `app_core`.
  `core/Core.hpp` no longer includes the 13 subsystem headers
  (database/pqxx, cache/redis++, jobs, Kafka, PayPal, mailer, storage,
  OTel/prometheus, ...) — those live in `Core.cpp`; `api/Middleware.hpp` no
  longer pulls the OTel SDK, spdlog or the security module headers into
  `Api.hpp` includers (only the drogon request/response types survive in
  signatures). File-local state (the middleware metric families, HSTS
  values, `global_app`) moved to anonymous namespaces in the `.cpp` files;
  `check-module-deps.sh` now allowlists `core/Core.cpp` as Core.hpp's own
  body file. No behavior change.

- Phase 1 hub split: module flags live in a ~50-line `core/Modules.hpp`
  (controllers no longer pull Kafka/PayPal/OTel headers to check a role),
  `api/Api.hpp` is included by the two server binaries only, the
  webhooks→email include edge is gone (`utils/CurlInit.hpp`). `make
  test-quick` now really rebuilds (the old no-rebuild behavior moved to
  `make test-rerun`); `make test-local NAME='Foo*'` documented as the inner
  loop.
- The utils hubs are de-inlined too (`Config`, `Strings`, `ErrorResponse`,
  `Crypto`, `api/Api`) — OpenSSL is confined to `Crypto.cpp`; Swagger UI is
  pinned to `swagger-ui-dist@5.32.14` with SRI integrity hashes.
- CI: C++ compiles go through sccache backed by the Actions cache (warm
  build-and-test ~17 → ~11 min), and the five heavy jobs self-scope — they
  always start, compute the changed-file set themselves and exit green in
  seconds when nothing they cover changed (no `paths:` filters, so no
  skipped-job-reports-success trap). clang-tidy carries its measured
  baseline (364 unique warnings) instead of an eternal TODO.
- `new-resource.sh`/`new-endpoint.sh` emit the post-refactor patterns
  (RepoErrors bases so `with_repo_errors` maps 404/409, `require_valid_uuid`,
  `Response::paginated/created`, `throw_on<E>`); the generated admin test
  authenticates (the harness runs auth.mode=jwt).
- `src/` quality pass: repeated guard/JSON/SQLSTATE blocks collapsed into
  shared helpers, dead includes dropped, hot-path copies removed (JWT claims
  moved not copied, request bodies handled as views). 49 files, behavior
  bit-for-bit.
- Dead template surface resolved: undocumented unused Auth/Messaging helpers removed; `CrudBase` owned-templates, `Database::execute_transaction`/`execute_write_idempotent` and friends are now instantiated by tests (which immediately exposed that the owned-templates required the templated `from_row` convention); `Validation::uuid` reuses the single UUID implementation.
- The app_core de-inline is complete: 22 more header/body pairs (all controllers, email, security, storage, observability, cache, Migrations), headers down 62%; the email↔jobs include cycle is gone from the header plane.
- CI sanitizers (ASan+UBSan and TSan) now cover the integration+api buckets against real Postgres/Redis, not just unit — the first instrumented run caught and fixed a latent heap-use-after-free (range-for over a temporary json).

### Added
- Billing SPA pages — `/billing` (wallet, packages, top-up), `/billing/return`,
  `/billing/cancel`, `/admin/billing` (packages/payments/settings) + a Wallet
  card with adjust dialog on the admin user page. Nav shows billing only when
  the API answers (probe-by-request); the fork's recharts metrics dashboard
  stays behind as a documented extension point — no new dependencies.
- Multi-tenancy starter: `scripts/add-orgs.sh` installs organizations,
  membership, org-claim JWT mint/switch, deny-by-default permission matrix
  and `Tenancy::OrgCrudBase` (structural read isolation — global finds do
  not exist); `new-resource.sh --org-scoped` scaffolds org-scoped resources.
  Passive template part: optional `org` claim on `AuthPrincipal`.
- Gate selftest `scripts/check-selftest.sh`: 18 planted breakages across all
  eight check gates, each must be caught AND named on every CI run (plus a
  nightly unconditional backstop, `.github/workflows/gates-nightly.yml`).
- Opt-in rendered-artifact gate skeleton for document-producing forks:
  `scripts/check-artifact.py` + `render-artifacts.sh` + mandatory selftest
  and `docs/RENDER-GATE.md` (generalized from the cyber-accountant fork).
- `scripts/release.sh <ver>` edits every version point (CMake, 9 helm image
  pins, 4 Chart appVersions) in one counted pass; `check-version-sync.sh`
  now gates the helm pins too. The frozen `appVersion: 1.4.0` (the default
  image tag for standalone chart installs) is unfrozen and gated.
- `scripts/check-config-sync.sh`: every `cfg.get` triple must exist in
  `config.json`, `config.sample.json` and `docs/CONFIG.md`; its first run
  found and fixed 78 real drifts (33 keys missing from config.json, stale
  doc defaults).
- `scripts/check-module-deps.sh`: the include graph between src/ directories
  is declared in `docs/module-deps.txt` and gated (62 edges; hard rules —
  `utils` imports nothing, `Core.hpp` is includable by three files only).
- `Database::install_for_testing` seam (mirrors Cache): repository-level
  logic is now unit-testable without Postgres via `detail::ErasedTxn`.
- Billing module (`BILLING_ENABLED`, off by default) — ported from the site
  fork (#24): PayPal checkout top-ups credited to an append-only wallet
  ledger (`src/billing/Wallet.hpp`, migrations 007/008), signature-verified
  PayPal webhook (capture/refund/reversal, idempotent redelivery), admin
  surface (package catalogue CRUD, runtime rate/bounds settings, payments
  list, manual wallet adjustments with audit trail, business-metrics
  aggregate), and best-effort transactional emails (top-up receipt,
  refund/reversal notice, failed-payment notice, opt-in `notify`
  adjustment notice) routed through the generic `email.send` job —
  delivery can never affect the money path. Full OpenAPI coverage and
  api/integration test suites included.
- `Billing::spend` — idempotent credit spending with a partial unique reference index (migration 009) and typed `InsufficientCredits`; the wallet test suite gains a randomized 300-op invariant check (`SUM(ledger) == cached balance` after every step).
- Changelog fragments: a PR now records its changelog entry as its own file
  `changelog.d/<topic>.<type>.md` (type ∈ added|changed|fixed|removed|security,
  format: `changelog.d/README.md`) instead of competing for the same
  `[Unreleased]` lines — `scripts/release.sh` folds the fragments in and
  retitles `[Unreleased]` itself, `./scripts/assemble-changelog.sh --check`
  gates fragment format in CI (fragments stay optional, direct `[Unreleased]`
  edits remain legal), and the gate selftest grows to 20 planted breakages.
- `.devcontainer` runs on the prebuilt `builder:cache` image — the native `make test-local` inner loop works minutes after opening (warm re-run ~7 s). `Makefile` build jobs are now RAM-capped (`JOBS`), fixing OOM kills on 8 GiB Docker VMs.
- The e2e suite validates every real JSON response against the OpenAPI schemas (dependency-free subset validator + committed spec JSON with a freshness hash); the first pass fixed the spec's missing `status` field, three long-standing YAML flow-map errors, four undocumented statuses and the absent `Post` schema.
- Nightly libFuzzer runs over the four byte-facing parsers (traceparent, decimal-cents, config placeholder expansion, path matching) with round-trip oracles and a growing cached corpus; harnesses are std-only by design — no vcpkg needed.
- `init-project.sh --with-orgs` (installs the multi-tenancy kit post-rename) and `--minimal` (strips the content module via `remove-content-module.sh` with the full gate battery green on the cut tree); self-verification fixed on pristine trees (#57).
- Opt-in transactional outbox (`Outbox::enqueue` in the caller's transaction + `SKIP LOCKED` drain into the jobs queue, migration 010, `outbox.drain_interval_sec` default off) for events whose loss after commit is unacceptable; decision guide in CONVENTIONS gotcha 20.
- Releases ship SPDX SBOMs and cosign keyless signatures/attestations for all three images; a nightly guard re-hashes the pinned Swagger UI assets against the CDN, and Renovate watches the pin without automerging it.
- `scripts/sync-upstream.sh` + a `.template-version` stamp: degit forks with no shared git history can pull template releases as three-way patches with honest conflict markers (`.template-sync-ignore` excludes renamed paths).

## [1.5.5] — 2026-08-21

### Fixed
- The `db_replica_lag_seconds` refresh (every 15 s, forever) ran through the
  traced read path and minted an orphan root `db.read` trace per tick —
  ~5.7k garbage traces/day per pod, spotted minutes after pointing the demo
  at the cluster Tempo. Internal maintenance reads now use a raw replica
  connection (new `Database::with_replica_connection`, the replica twin of
  `with_primary_connection`) — no tracing, same LSN-comparing query.
- The demo preset pins `mi-fitness-api.app.baseUrl` — without it the sitemap and
  account-email links advertised the chart-default `app.example.com`
  (caught on the demo's first sitemap render after enabling content).
- `deploy-demo.sh` execs into the newest RUNNING api pod — right after a
  rollout the label selector still matched the old replica set's
  terminating pod and `create-admin`/`seed` died with "container not
  found".

## [1.5.4] — 2026-08-21

Wave 2 — "downstream hardening": a three-fork audit (site, cyber-accountant,
tarassov.me) backported every generic fix and gate the forks paid for
(spec/plan in `docs/superpowers/`, PRs #17–#23, #27).

### Security
- `.gitleaks.toml` gained the load-bearing `[extend] useDefault` — a config
  carrying only an `[allowlist]` runs gitleaks v8 with ZERO rules, so the
  secret scan had been vacuously green. Allowlist is value-scoped; work-tree
  scans use the new `.gitleaks.worktree.toml`. (#17)
- Helm umbrella credential defaults are EMPTY (deploys pass them via `--set`;
  the API fail-closes on an empty/short JWT secret). The previous public
  placeholder JWT key was silently inherited by a downstream production
  release and verified real tokens. `check-helm-render.sh` now asserts no
  tracked overlay renders a non-empty credential Secret. (#17)
- Umbrella placeholder Postgres/api/app-ingress render behind
  `postgres.enabled` / `ingress.enabled` guards. (#17)
- Email HTML templates escape every context string (inja has no autoescaping);
  `build_mime` strips CRLF/quoted-string metacharacters from From/Subject
  metadata; `--dump-config` masks credentials. (#18)

### Fixed
- `check-test-buckets.sh` scanned only its first directory argument —
  `tests/api/` was never checked. `CI_REQUIRE_INFRA` is now actually exported
  in CI (missing test infra fails loudly instead of SKIP-and-green). App
  version un-stuck from 1.4.0 (three releases stale) and gated by
  `check-version-sync.sh`; release builds stamp the git tag into the binary.
  (#19)
- Image tag pins matched to what release.yml actually publishes (unprefixed;
  the `v1.4.0` pins pointed at GHCR tags that never existed). (#17)
- Short-circuited middleware responses (401/403/415/429/idempotency-replay/
  CORS preflight) now carry X-Request-Id, security headers, an access-log
  line and a metric sample — they used to bypass the whole pre/post chain.
  Content-Type comparison is case-insensitive per RFC 9110. `Config::get<T>`
  no longer swallows type errors (string leaves coerce like env vars; garbage
  logs at ERROR). Primary and replica DSNs get a bounded `connect_timeout`.
  Wrong-typed JSON fields (e.g. `{"password":123}`) return the 400 envelope
  instead of a bare 500, and a global exception handler keeps the canonical
  error shape on paths outside `with_repo_errors`. Nav highlights on path-
  segment boundaries. Stale pre-versioning `/api/jobs` references swept
  (`smoke.sh` assertion could never match). (#21, #27)
- Migrations record a sha256 checksum; editing an applied migration now fails
  boot with an explanation instead of silently never reaching migrated
  databases. (#20)

### Added
- `REDIS_DB` / `cache.db` logical-database isolation end-to-end (standalone,
  Sentinel, worker BRPOP client, compose, both charts, docs) for sharing one
  Redis instance between apps. (#21)
- `api.public_paths_extra` — ADDITIVE public-paths key; the full-override
  `api.public_paths` trap 401'd content-module routes and a fork's payment
  webhook twice. (#27)
- `scripts/new-module.sh` — scaffolds a feature-module master switch through
  config, `Core::<name>_enabled()`, compose and both helm charts in one
  command. 4-arg `with_repo_errors(cb, op, fn, after_fn)` for after-response
  side effects that can never double-fire the callback. `auth.csrf.enabled`
  in the mi-fitness-api chart. (#27)
- CI: `runtime-smoke` job (builds+starts the release images, checks `ldd` and
  the CA bundle), frontend nginx compose↔helm drift gate. (#19)
- `docs/UPSTREAM.md` (fork↔template sync + backport-candidate discipline) and
  `docs/CI-PROFILES.md` (hosted vs self-hosted runners, arm64 verdict).
  (#22, #23)

### Changed
- inja is vendored (`third_party/inja-include`, SHA256-asserted) instead of
  downloaded at configure time — GitHub 429s could stop CI from configuring.
  `make fmt`/`lint-format` refuse to run with a non-17 clang-format major
  (the CI pin). vcpkg clone/install retried 3× in the Dockerfile. Heavy CI
  jobs 60→90 min so a cold vcpkg rebuild can finish and export its cache.
  (#23)
- `release.yml` publishes under `ghcr.io/${{ github.repository }}` — forks
  release under their own namespace with zero edits. `autofix.yml` syncs the
  frontend lockfile before `npm ci`. `init-project.sh` regenerates
  `helm/*/Chart.lock` after chart renames and rewrites the Makefile
  `GHCR_ORG` default. (#22)
- Test fixtures no longer bind a Prometheus exposer (a flat ~2 s TearDown tax
  per Core-backed test) and default to `auth.mode=jwt` so RBAC guards are
  actually exercised (they were no-ops under `none`). Multi-table cleanup
  goes through `TestHelpers::wipe_app_data()`. Upload 400-paths and storage
  list()/magic-byte validation tests ported from the content module's origin
  fork (closes #12). (#20)

## [1.5.3] — 2026-08-09

### Fixed
- Admin Jobs "Open trace" no longer guesses the tracing UI host by swapping
  the first DNS label for `jaeger` — on an apex domain that produced a link
  to a stranger's site. The target is a runtime template containing
  `{traceId}` (cpp-frontend chart's `traceUi`, served as `/trace-ui.js`), so
  one image works everywhere and Grafana/Tempo deep links are supported.
  Unset → no button; all-zero trace ids no longer render a dead link.
- `secret-scan` runs the open-source gitleaks CLI instead of the action that
  hard-fails without a paid license on organization-owned repositories.

## [1.5.2] — 2026-08-09

### Fixed
- `db_replica_lag_seconds` reported "time since the last replayed COMMIT",
  which climbs without bound on an idle primary even when the replica is
  fully caught up — firing `CppApiHighReplicaLag` permanently on any quiet
  deployment. Receive/replay LSNs are compared first and the metric reports
  0 when they match.

## [1.5.1] — 2026-08-09

Live-deploy fixes for the 1.5.0 content module.

### Fixed
- Frontend nginx now proxies the content module's public routes
  (`/sitemap.xml`, `/posts/*`, `/uploads/*`) to the backend — they previously
  fell through to the SPA catch-all and served `index.html`.
- The mi-fitness-api chart gained the missing uploads-storage wiring
  (`values.storage` → config.json → Secret → `S3_ENDPOINT`/`S3_SECRET_KEY`);
  a Helm deploy could previously only run the pod-local backend.
- `client_max_body_size 6m` on the frontend `/api/` proxy — nginx's 1m
  default 413'd photo uploads before they reached the backend.
- The JSON content-type gate exempts `multipart/form-data` — it previously
  415'd every upload before UploadController's own validation ran.
- Runtime image ships `ca-certificates` — without the CA bundle every
  outbound TLS from the app failed (first hit: SMTP STARTTLS to the mail
  relay, "Problem with the SSL CA cert").

## [1.5.0] — 2026-08-08

Content module — an optional posts/blog subsystem backported from a
production fork, plus CI/infra hardening accumulated since 1.4.0.

### Added
- **Content module** behind `content.enabled` (default **off**, env
  `CONTENT_ENABLED`, wired through Helm/compose/docs): posts with admin CRUD
  (`/api/v1/posts*`) and HMAC preview tokens; public JSON feed
  (`/api/v1/public/posts*`); articles served as raw Markdown at
  `GET /posts/{slug}` (drafts via `?preview=`); dynamic `/sitemap.xml`
  (root-only degrade when the module is off); admin uploads API with local
  and S3 (SigV4) storage backends and public `/uploads/*` serving
  (posts/-prefixed, extension + magic-byte validated). New public routes ride
  the strict rate-limit tier. SPA gains Posts and Media tiles on the
  `/admin` dashboard. Migration `006_add_posts.sql`.
- Nightly benchmark trend: `bench-nightly.yml` publishes wrk + footprint
  series to gh-pages with >30% regression alerts (trend-only, never gates
  PRs); README badge and BENCHMARKS.md section.
- `autofix.yml` — dispatchable clang-format + OpenAPI TS schema regeneration
  that pushes the fix commit back to the branch.

### Fixed
- e2e harness startup race: HTTP clients now run on a dedicated event loop
  instead of racing `app().run()` for the lazily-bound framework loop.
- Stale pre-`/api/v1` `/api/jobs` references in bench harness defaults,
  README examples and Makefile help (the old default benchmarked a 404).

### Changed
- Docker build: vcpkg dependencies compile in their own layer before
  `COPY . .` — source-only changes no longer pay the ~35-minute cold
  dependency build in CI or locally.
- CI reads the GHCR builder cache as a fallback and gains a ThreadSanitizer
  job; coverage floor raised to 54% with a documented ratchet rule.

## [1.4.0] — 2026-06-26

Fork-readiness hardening + API versioning. Prepares the template to be made
public and forked into production.

### ⚠ Breaking
- **All API routes are now versioned under `/api/v1`** (ADR 0006). Clients must
  call `/api/v1/...`; the bundled frontend moves in the same release. There is
  no compatibility alias — deploy backend and frontend together. Probe/infra
  routes (`/healthz`, `/ready`, `/health`, `/metrics`) stay unversioned.

### Added
- API versioning convention, machine-enforced: `new-resource.sh` derives the
  route from a single `API_VERSION` var, `new-endpoint.sh` rejects unversioned
  paths, and `check-routes-registered.sh` lints for them. ADR 0006 documents it.
- Jobs: exponential retry backoff + a cross-worker visibility-timeout reaper;
  a startup guard that refuses/​warns on a `WORKER_TYPES` entry with no handler.
- Health: degraded (non-critical) probes — surfaced in `/health` without failing
  `/ready`.
- Observability: `trace_id` on every log line; PrometheusRule alerts for api/worker.
- Boot-time prod-safety checks in the binary (rate-limit off, docs UI on, CSRF
  off, weak DB password) that the Helm deploy path can't bypass.

### Changed
- DB connection string is assembled from discrete env parts so the password is
  never materialized into a URL env var; request-path DB retry defaults tightened
  so a transient blip can't stall the IO loop.
- Migrations support a `-- migrate:no-transaction` marker (autocommit, cleared
  statement_timeout) for `CREATE INDEX CONCURRENTLY` / large backfills.
- Helm ships structured JSON logs in prod; frontend drops production sourcemaps;
  critical CI images pinned by digest; emails masked in logs.
- `init-project.sh` scrubs the author's demo/infra identifiers on fork.

### Fixed
- API-key auth throttles the `last_used_at` write (CTE) instead of writing on
  every request. Numerous review-driven correctness fixes (see PRs).

## [1.3.4] — 2026-06-25

Hardening and reliability from a full project review.

### Added
- Jobs: opt-in exponential retry backoff plus a cross-worker visibility-timeout
  reaper that reclaims jobs orphaned by a crashed worker (enabled in production).
- Health: degraded (non-critical) probes — surfaced in `/health` but never fail
  `/ready`, so an optional dependency outage no longer pulls the pod from rotation.
- CI: Trivy scans for the worker and frontend images, a coverage job, frontend
  ESLint in GitLab, and full-history gitleaks.

### Changed
- Frontend: theme bootstrap moved to an external script so the production CSP no
  longer blocks it; admin pages are code-split.
- Helm: frontend and the demo-reset job run with a read-only root filesystem;
  api/worker gain a `startupProbe`.

### Fixed
- Frontend: corrected the admin permission sentinel shown in the UI
  (`0x40000000`, not `0xff`); reverse-tabnabbing and a dialog a11y gap.
- Production deploy: re-enable rate limiting and stop double-running migrations.

## [1.3.3] — 2026-06-25

Frontend redesign + the real fix for notification layout shift.

### Changed
- **Dark-first "dev-tool" redesign**: zinc neutrals + a single indigo accent,
  tighter radius/spacing, borders over shadows, driven by the `index.css`
  design tokens. Dark is now the default theme (light still available via the
  toggle). Polished Card / Nav / DataTable / job-status badges to match.

### Fixed
- **Notifications no longer shift the layout.** Server/form feedback (invalid
  login, saved, etc.) now shows as a TOAST (`position: fixed`, out of document
  flow) instead of an in-flow alert that pushed the form down. Field-level
  validation stays inline. New `useToast` / `useErrorToast`; `FormAlert`
  removed.

## [1.3.2] — 2026-06-24

Frontend UX/accessibility overhaul (a multi-agent review of the SPA) plus
real-client-IP plumbing fixes. No backend API changes.

### Changed
- **Frontend UX + a11y overhaul** (12 items): form errors no longer jolt the
  layout — a new `FormAlert` renders an always-present, `aria-live` slot that
  smoothly expands/collapses instead of inserting an Alert and shoving the
  fields down (CLS), applied across all forms. Unknown routes now show a 404
  page (was a silent redirect home). Nav gains an active-link indicator and a
  mobile hamburger menu. Reset/change/request forms moved to the shared
  `useApiMutation` (consistent pending/error handling). Admin forms use the
  accessible `FormField`; destructive deletes use a focus-trapped
  `ConfirmDialog` instead of native `confirm()`. DataTable shows skeleton rows
  on first load; tables get `scope="col"`. Dark-mode-safe job badges, explicit
  zod validation messages, responsive name grids, skip-to-content link, focus
  rings, and aria-labels on icon buttons / pagination.
- Audit detail now opens in a centered, focus-trapped modal (was appended at
  the bottom of the page, past the table).

### Fixed
- **Real client IP through the frontend path**: the cpp-frontend nginx proxied
  `/api` with `X-Real-IP $remote_addr`, clobbering the real client IP with the
  ingress pod's internal address — requests via `app.<host>` audited a `10.x`
  IP. It now passes the upstream `X-Real-IP` through.
- Helm: `RATE_LIMIT_TRUST_PROXY` is always rendered (was gated on
  `rateLimit.enabled`), so the audit honors `trust_proxy` even with rate
  limiting off.

## [1.3.1] — 2026-06-24

Patch release: trusted client-IP handling and two Helm deploy fixes.

### Fixed
- **Audit honors `trust_proxy`**: the failed-login audit read `X-Real-IP`
  directly (trusting a client-spoofable header even when not behind a proxy) and
  diverged from the rate limiter. Added `RateLimit::client_ip(req)` — resolves
  `rate_limit.trust_proxy` / `trusted_proxy_count` from config (works even when
  rate limiting is disabled) and applies the same trusted-IP logic; the audit
  now uses it. Capturing the *real* client IP still requires the edge to forward
  it (PROXY protocol on the LB + ingress-nginx) and `trust_proxy=true`.
- **Helm**: guard the mi-fitness-api `mail-smtp-password` Secret against a nil `mail`
  map, so `helm upgrade --reuse-values` (whose reused values omit the optional
  mail block) no longer fails to render.
- **Helm**: pin the cpp-env umbrella + demo image tags to the v-prefixed release
  tag (`v1.3.x`); a bare `1.3.0` matched no published image.

## [1.3.0] — 2026-06-24

Fork-readiness round: programmatic auth, outbound integrations, a storage seam,
and a batch of security/scaffolding hardening ahead of building the first real
project on the template.

**Heads-up — admin permission model changed.** `Permission::kAdminister` moved
from `0xff` to a dedicated sentinel bit (`0x40000000`). It is data-migrated
automatically (migration 004 converts the seeded `Administrator` role), so no
manual action is needed — but if you persisted the old `0xff` admin value
anywhere outside the seeded role, run the same `UPDATE`. Everything else is
additive: new config keys have defaults / env fallbacks, so existing configs
load unchanged.

### Added
- **API keys / personal access tokens** for machine clients (migration 005).
  `Security::ApiKeys::generate/authenticate`; presented via `X-API-Key` or a
  `cpk_`-prefixed `Authorization` token; only the SHA-256 hash is stored.
  Owner-scoped management at `POST/GET/DELETE /api/account/api-keys` (a user
  only ever sees/revokes their own; the secret is shown exactly once).
- **Outbound webhooks**: `Webhooks::send()` enqueues a signed (`X-Webhook-
  Signature: sha256=…` HMAC) POST delivered by the worker with retry/backoff/DLQ.
  Coarse SSRF guard refuses localhost / private / link-local / cloud-metadata
  hosts and non-http(s) schemes; redirects disabled.
- **Object/file storage seam**: `Storage::get()` (put/get/remove/exists/url)
  with a `LocalStorage` disk backend and a path-traversal guard. Swap in S3/GCS
  by subclassing `StorageBackend`. Config under `storage.*`.
- **Generic transactional email**: `Email::SendEmail::send(to, subject, text,
  html)` enqueues an `email.send` job for any ad-hoc mail (not just account
  flows).
- **Owner-scoped resources**: `new-resource.sh --owned` scaffolds an IDOR-safe
  per-user resource (owner FK migration, `CrudBase::find_owned/list_owned/
  count_owned`, `API_REQUIRE_OWNER`). The scaffolder also emits a no-infra
  domain unit test alongside the integration test.
- `Cache::cached<T>(key, ttl, loader)` read-through helper (fail-open).
- **Replica-lag observability**: `db_replica_lag_seconds` gauge +
  `CppApiHighReplicaLag` alert (only active when replicas are configured).
- Tracked, secret-free `values-prod.example.yaml` for the api / worker /
  frontend charts; `init-project.sh --no-demo` strips the flask-base reference
  material; `REMOVING-THE-DEMO.md`.
- CI gates: `CI_REQUIRE_INFRA` fails (vs skips) when integration infra is
  absent; `check-routes-registered.sh` asserts every `ADD_METHOD_TO` is in the
  endpoint registry; `check-helm-render.sh` asserts the prod overlay's security
  floor (rate-limit fail-closed, JWT, secure cookies, no committed secrets).

### Changed
- **Admin is a dedicated sentinel bit, not `0xff`** — a role accumulating the
  eight low feature bits can no longer accidentally become admin. `current_user_
  can` (and the TS mirror) give the sentinel an explicit allow-everything bypass
  so admins still pass feature gates. Migration 004 converts existing admin rows.
- `server.threads` default is now `0` = auto (one IO thread per core); the app
  warns at boot when `database.pool_size < threads` (the real DB-concurrency cap
  is the thread count, not the pool).
- Every container drops **all** Linux capabilities (`capabilities.drop: [ALL]`).
- Worker queue-type defaults include `email.send` and `webhook.deliver`.
- Docs reconciled with the CrudBase scaffolder (EXAMPLES / CONVENTIONS); the
  inaccurate "multi-arch" GitLab-CI claim corrected with deploy-arch guidance;
  `make warm-cache` falls back to the upstream GHCR cache and `make doctor`
  probes Colima memory. `app_core` introduced as the single CMake dependency seam.

### Fixed
- **Failed login attempts are now audited** (`auth.login_failed` with the
  attempted email + source IP) — brute-force was previously invisible.

### Security
- Privilege-escalation prevented via the admin sentinel bit (above).
- API-key secrets hashed at rest; revocation is owner-scoped and returns 404 for
  others' / missing keys (no enumeration oracle).
- Webhook SSRF guard; storage path-traversal guard; containers drop all caps.
- Prod overlay ships the rate limiter **fail-closed**, now CI-asserted.

## [1.2.0] — 2026-06-23

Security hardening, saturation observability, and template/DX fixes. No
breaking API changes — all new config keys have defaults / env fallbacks, so
existing config files load unchanged.

### Added
- **Saturation metrics**: `db_pool_active_connections` + `db_pool_size`
  (labeled by pool — the answer to "is the pool about to time out on
  acquire?") and `jobs_queue_depth` (the waiting queue — a *leading* indicator,
  unlike the lagging `jobs_dlq_depth`). New Prometheus alerts
  `DbPoolSaturationHigh` and `JobsQueueBacklog` with RUNBOOK entries
  (`#dbpool`, `#queuebacklog`) and the matching SLO rows.
- **Opt-in CSRF** double-submit guard (`security.csrf.*`, off by default):
  server middleware verifying a token header against a non-HttpOnly cookie on
  cookie-authenticated mutations, with the SPA client wired to echo it.
- **Stricter rate-limit tier** config for the auth surface
  (`rate_limit.protected_requests` / `protected_window_sec` /
  `protected_paths`).
- `make` front doors: `make new-resource`, `make new-job`, `make init`.
- Coverage gate: `make coverage` now fails under `COVERAGE_MIN`% line coverage
  (default 40, a regression floor — override per-run).
- `scripts/init-project.sh` gains an optional `[domain]` argument to rebrand
  the host alongside the project name.

### Changed
- `with_repo_errors` is decoupled from the User/Role domain: repository
  exceptions now derive from generic `Repositories::NotFoundError` /
  `ConflictError` bases (`repositories/RepoErrors.hpp`), so the shared
  controller plumbing no longer includes the demo repositories and a forked
  domain's own exceptions map to the right status automatically.
- `new-resource.sh` scaffolds the repository on `CrudBase` (the base whose
  whole purpose is `find` / `list` / `count`) instead of hand-rolling them.
- The scaffolders are now discoverable where forkers read — README "first
  steps", `docs/INDEX.md`, and the Make-targets table — and `new-job.sh` is
  executable.
- `init-project.sh` verifies the rename itself (an independent broad scan that
  exits non-zero listing any survivor) instead of printing a grep for you to
  run.

### Fixed
- **Single-source version**: `CMakeLists.txt project() VERSION` was left at
  `1.0.0` through the 1.1.0 release, so the binary mis-reported its version;
  it now tracks the release.
- Dead scaffolding instructions removed: the generated test + `new-resource.sh`
  trailer + `docs/CONVENTIONS.md` no longer point at a non-existent
  "INTEGRATION_FILTER / 5 places" bucket registration (buckets are classified
  by directory).
- `init-project.sh` no longer leaves the template name behind in the vcpkg
  manifest (`vcpkg.json` was excluded by an over-broad `./vcpkg*` filter),
  `.env.*` variants, and helm `NOTES.txt` — nor the author's `security@` /
  demo host in a fork.

### Security
- **The public auth/account surface is now rate-limited.** `api.public_paths`
  exempted login / register / refresh / password-reset from *both* auth and the
  limiter, leaving them open to credential brute-force and mail-bombing. A
  second, stricter per-IP tier (separate `rl:auth:` namespace) re-arms them; the
  production profile enables it by default (10 req/60 s). Health/metrics/static
  stay unthrottled. **Behavior change on upgrade:** a burst against the auth
  endpoints now returns 429 — tune via `RATE_LIMIT_PROTECTED_REQUESTS`.
- **Baseline HTTP security headers** on every response — `X-Content-Type-Options:
  nosniff`, `X-Frame-Options: DENY`, `Referrer-Policy: no-referrer`, a
  locked-down CSP, and opt-in HSTS (`security.hsts`, on in the production
  profile) — set both in the app and at the nginx edge for the SPA.

## [1.1.0] — 2026-06-23

Pre-release hardening + a public demo. No breaking API changes.

### Added
- Public demo environment at `*.demo.tarassov.me` — `helm/cpp-env/values-demo.yaml`
  + `scripts/deploy-demo.sh` (external-dns + cert-manager, Mailpit/Jaeger UIs),
  with a periodic reset CronJob that wipes and reseeds the data.
- `THIRD_PARTY_NOTICES.md` (dependency licenses + flask-base attribution),
  `docs/TESTING.md` (what the suite covers and doesn't), `docs/BENCHMARKS.md`
  (how to measure latency/footprint), `CODE_OF_CONDUCT.md`, and GitHub issue
  templates.
- Working dark-mode toggle and a real favicon/brand in the SPA.

### Changed / Fixed
- **Security:** constant-time bearer-token compare; the production auth-guard is
  now actually armed (`APP_ENV` wired through config + Helm); the mi-fitness-api chart
  defaults to `auth.mode=jwt` so a bare install can't ship a public API.
- **Fork experience:** `make quickstart` / `up` build the fork's own code instead
  of pulling the upstream image; neutral registry default; CODEOWNERS and
  Prometheus runbook links no longer hardcode the author's identity;
  `init-project.sh` rebrands the GHCR namespace.
- **Ops:** the backup CronJob verifies the dump (`gunzip -t` + size guard) instead
  of silently shipping a truncated one, with failure/staleness alerts;
  `seccompProfile` on all pods; single-replica PodDisruptionBudgets no longer
  wedge node drains; `make coverage` runs every test bucket.
- **CI:** Trivy fails on HIGH as well as CRITICAL; all GitHub Actions pinned to
  SHAs; the tag-release image job promotes the natively-built per-arch images
  into the `:vX.Y.Z` manifest instead of a QEMU rebuild (which segfaulted
  emulating amd64).
- **Docs:** corrected the README inventory, a non-existent module in an ADR, a
  migration-skeleton trap, dead clone/CI links, and a CHANGELOG contradiction.

## [1.0.0] — 2026-06-10

First tagged release. Highlights of the pre-release hardening pass:

- HTTP end-to-end test layer (real Drogon server + client) covering the
  middleware chain: auth gate, cookie sessions on the wire, refresh
  rotation/revocation, idempotency replay, permission bitmask, tracing.
- Frontend session refresh (401 → /api/auth/refresh → retry) + first
  frontend unit tests; committed package-lock.json for reproducible builds.
- Single-source version (CMake → version.hpp), production config profile
  with `make prod-check`, Prometheus alert rules, Renovate, prebuilt
  builder-image cache (`make warm-cache`).
- Test-bucket filters by explicit suite names; deflaked tamper/expiry
  tests; precompiled headers.

### Added
- Account / Auth / Admin feature surface modeled after flask-base
  (see `docs/PATTERNS-FROM-FLASK-BASE.md` for the file-level mapping):
  - `migrations/001_users_and_roles.sql` — users + roles + permission
    bitmask. Bits match flask-base's `Permission` class
    (`GENERAL=0x01`, `ADMINISTER=0xff`).
  - `src/domain/{User,Role}.hpp` + `src/repositories/*` — typed DTO +
    SQL-only repositories with `DuplicateEmail` / `UserNotFound`
    typed exceptions.
  - `src/security/Password.hpp` — argon2id password hashing via
    libsodium (replaces flask-base's PBKDF2).
  - `src/security/Tokens.hpp` — HMAC-signed timed link tokens with
    per-purpose key derivation (replaces flask-base's
    `URLSafeTimedSerializer`).
  - `src/security/Auth.hpp` — JWT in HttpOnly cookies (`__Host-access`
    + `__Host-refresh`), refresh-token JTI revocation in Redis,
    `current_user_can(req, perm)` / `require_admin(req)` helpers.
  - `src/api/AuthController.hpp` — `/api/auth/{register,login,logout,refresh,me}`.
  - `src/api/AccountController.hpp` —
    `/api/account/{confirm-resend,confirm/{token},reset-password-request,
    reset-password/{token},change-email-request,change-email/{token},
    change-password}`.
  - `src/api/AdminController.hpp` — `/api/admin/{users,users/{id},
    invite,roles}` with self-protection guards.
  - `src/email/Mailer.hpp` — SMTP outbound via libcurl.
  - `src/email/Templates.hpp` + `templates/email/*.{html,txt}` — inja
    rendering for confirm / reset_password / change_email / invite.
  - `src/email/AccountEmails.hpp` — token-issuing senders shared by
    Auth + Account + Admin controllers.
  - CLI ops: `--setup-dev`, `--create-admin EMAIL [PASS]`,
    `--seed-fake [N]`. flask-base parity: `manage.py setup_general /
    add_fake_data`.
  - Mailpit dev sidecar (`docker-compose.yml`) on :8025 (UI) / :1025
    (SMTP). No TLS, no auth — local-only catcher.
- React SPA under `frontend/`:
  - Vite + React 18 + TypeScript + Tailwind + shadcn/ui (Radix
    primitives) + TanStack Query + react-hook-form + zod + Zustand.
  - openapi-fetch typed client driven by `npm run gen:api` against
    `docs/openapi.yaml`. Stub committed so a fresh clone compiles.
  - Pages: Home, About, Login, Register, CheckEmail, ConfirmEmail,
    Unconfirmed, Profile, ChangePassword, ChangeEmail, RequestReset,
    ResetPassword, Admin (Dashboard, Users, UserDetail, InviteUser).
  - `<ProtectedRoute>` with `requireConfirmed` and
    `requirePermission={Permission.Administer}` guards; `<Layout>`
    runs `useMe()` once so every page has a fresh principal.
  - `frontend/Dockerfile` (multi-stage Node→nginx) +
    `frontend/nginx.conf` proxy `/api/* -> app:8080`. New
    docker-compose service exposed on host :3001.
- Make targets: `frontend-{install,dev,build,lint,format,typecheck,
  test,gen-api,up,image}`.
- CI: `frontend` job (typecheck + lint + production build) +
  `openapi-drift`, `clang-tidy`, `sanitizers` jobs (paritet with
  GitLab CI).
- ADR-0005 documenting the SPA split.
- `docs/PATTERNS-FROM-FLASK-BASE.md` — authoritative list of what we
  lift from flask-base, what we change, and what we don't lift.
- `--run-migrations` CLI flag on the app binary (mirrors
  `RUN_MIGRATIONS_ONLY=1`; convenient for native dev / `make migrate-local`).
- New Make targets: `test-watch`, `migrate-local`, `ci-local`, `helm-lint`,
  `new-endpoint`, `new-migration`, `tail-trace TID=…`, `env-check`.
- `scripts/env-check.sh` — reports `${VAR}` placeholders in
  `config/config.json` that are unset and have no default (multi-token
  aware, handles `database.primary` correctly).
- `scripts/new-endpoint.sh --with-test` and `--patch-openapi` flags —
  scaffold a `tests/api/test_<name>.cpp` smoke test and patch
  `docs/openapi.yaml` in place instead of just printing a stub.
- `scripts/init-project.sh` idempotency guard — refuses to re-run with a
  different name unless `--force` is passed.
- `.editorconfig` — whitespace/EOL rules for editors that don't read
  `.clang-format`.
- `envrc.sample` — direnv template covering `VCPKG_ROOT`,
  `VCPKG_BINARY_SOURCES`, `TEST_PG_HOST`, `DATABASE_PASSWORD`.
- `docs/INDEX.md` — one-line navigator across docs / ADRs / scripts /
  configs.
- CMake: CTest labels (`unit` / `integration`) so `ctest -L unit` works
  without re-deriving the gtest filter; `ENABLE_WERROR` option for CI.
- GitHub Actions parity with GitLab: `openapi-drift`, `clang-tidy`, and
  `sanitizers` jobs.
- Repository pattern scaffolding in `docs/EXAMPLES.md`: typed DTOs with
  `nlohmann::to_json`, repositories that own all SQL and raise typed
  `DuplicateKey` / `UserNotFound` exceptions, thin controllers that
  translate those to HTTP status codes.
- `LOG_FORMAT=json` (default `text`) — emits one JSON record per line with
  JSON-escaped message via a custom spdlog flag; includes `service`,
  `thread`, `level`, ISO8601 timestamp for Loki/ELK/Datadog pipelines.
- CLI ops flags on the app binary:
  - `--print-routes` prints the endpoint table and exits (no subsystems).
  - `--dump-config` resolves config (JSON + env) and prints it as JSON.
  - `--verify-migrations` reports pending migrations, exits 1 if any pending.
- `MigrationRunner::list_pending()` — read-only migration diff helper.
- `Jobs::init_blocking_client()` for in-process tests so BRPOP has a socket
  timeout budget that matches its block timeout.
- `docker-compose.yml` `test-redis` service in the `test` profile — isolates
  the test suite from the dev-stack worker that would otherwise BRPOP
  test-submitted jobs off the shared queue.
- `DATABASE_REPLICA_URLS` passthrough in the worker service so the worker
  can route reads to the replica under `up-everything`.
- `.github/workflows/ci.yml` (full build + test + clang-format + shellcheck)
  and `.github/workflows/release.yml` (multi-arch GHCR push on `v*` tags).
- `migrations/README.md` starter doc — naming, `--verify-migrations`, ops.
- Env-var interpolation (`${VAR}` / `${VAR:-default}`) in config JSON values.
- `Config::require<T>()` — throws on missing required config.
- JWT (HS256) auth middleware with exp/nbf/iss/aud validation + RBAC helpers.
- Static bearer-token mode for dev convenience (`auth.mode=bearer`).
- Redis-backed fixed-window rate limiter with per-IP / per-user scope.
- Idempotency-Key middleware for POST/PUT/PATCH/DELETE.
- Dead-letter queue for jobs (`jobs:dlq:*`) with GET `/api/jobs/dlq` and
  POST `/api/jobs/dlq/{id}/requeue`.
- Generic retry-with-backoff utility (`Retry::run`) with pqxx / redis-plus-plus
  transient-error classifiers; applied to `Database::execute_read/write`.
- W3C Trace Context parsing and `X-Request-Id` / `traceparent` response
  headers; per-request trace-id attached to request attributes.
- Validation helpers (`Api::Validation::Errors`, `require`, `string_length`,
  `regex_match`, `int_range`, `one_of`, `email`, `uuid`).
- Graceful shutdown: readiness flips to 503 on SIGTERM; Drogon drain after
  configurable pre-stop delay; worker bounded drain.
- CMake option `ENABLE_SANITIZERS` (ASan + UBSan).
- `.clang-tidy` baseline + CI lint job.
- Sanitizer CI job (unit subset).
- Helm: `preStop` hook, `terminationGracePeriodSeconds`, external-secrets
  ExternalSecret skeleton, PrometheusRule with baseline SLO alerts.
- OpenAPI 3.1 spec under `docs/openapi.yaml`.
- Governance: `CODEOWNERS`, `SECURITY.md`, `CONTRIBUTING.md`, MR/PR templates.

### Changed
- `scripts/check-openapi-drift.sh` now compares `(method, path)` tuples,
  not just paths — catches verb-only changes (`GET → POST` on the same
  route) the previous diff missed.
- CMake test glob uses `CONFIGURE_DEPENDS` so a freshly-added
  `tests/<bucket>/test_*.cpp` is picked up on the next build without a
  manual `cmake --preset dev` reconfigure.
- Added stricter compile warnings by default: `-Wshadow`,
  `-Wnon-virtual-dtor`, `-Wold-style-cast`, `-Wcast-align`,
  `-Woverloaded-virtual`, `-Wnull-dereference`, `-Wdouble-promotion`,
  `-Wformat=2`. Debug build also gets `-fno-omit-frame-pointer`.
- `.devcontainer/devcontainer.json` `postCreateCommand` now calls
  `make compile-commands` instead of duplicating the cmake invocation.
- `Jobs::fail()` after `max_retries` now transitions jobs to status `dead`
  and pushes to the DLQ instead of a terminal `failed` state.
- `config/*.json` no longer contain plaintext passwords — use `${VAR}`
  placeholders.

### Removed
- Demo `users` / `posts` / `events` controllers, their repositories, domain
  types, tests, and migrations. The real auth / account / admin / audit domain
  stays; only the throwaway CRUD demo was removed, and the worked CRUD pattern
  moved to `docs/EXAMPLES.md` so `src/` stays free of example noise when you fork.
- `ensure_test_seed()` helper (no longer needed without the demo schema).

### Security
- Default `auth.mode` is `none` for local development, but the service
  refuses to start in `jwt` mode without a `JWT_SECRET` set.
- OpenSSL linked explicitly for HMAC-SHA256 (JWT signature) and SHA-256
  (Idempotency-Key body hash); constant-time compare via `CRYPTO_memcmp`.

[Unreleased]: https://gitlab.com/tarassov.me/mi-fitness-api/-/compare/v1.2.0...master
[1.2.0]: https://gitlab.com/tarassov.me/mi-fitness-api/-/compare/v1.1.0...v1.2.0
[1.1.0]: https://gitlab.com/tarassov.me/mi-fitness-api/-/compare/v1.0.0...v1.1.0
