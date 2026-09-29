# CLAUDE.md — agent guide for this repo

C++20 REST service template: Drogon + PostgreSQL + Redis, vcpkg/CMake,
React SPA in `frontend/`, Helm charts in `helm/`. `docs/INDEX.md` is the
map of all documentation; `docs/CONVENTIONS.md` is the pattern reference.

## Prime directive: scaffold, don't hand-roll

- Full CRUD resource: `./scripts/new-resource.sh Name` — migration + DTO +
  repository + controller + registry row + OpenAPI block + integration test.
  `--owned` = per-user rows; `--org-scoped` = per-tenant rows (requires the
  orgs kit below).
- Multi-tenancy starter (one-shot, NOT a runtime flag): `./scripts/add-orgs.sh`
  — installs src/tenancy/, org guards, orgs API + migration + tests
  (docs/ORGS.md). Also available at bootstrap: `init-project.sh --with-orgs`.
- Fork bootstrap: `./scripts/init-project.sh <name> [registry] [domain]` —
  `--no-demo` strips reference material; `--minimal` additionally runs
  `./scripts/remove-content-module.sh` (one-shot removal of the content
  module — posts/uploads/sitemap — via `init-project:content:*` marker
  blocks + pattern edits, gates stay green; REMOVING-THE-DEMO.md). When
  cutting content-module code into a shared file, keep it inside those
  markers. init-project's self-verification skips docs/superpowers/
  (historical archives keep pre-rename tokens verbatim).
- Single endpoint: `./scripts/new-endpoint.sh FooController Get /api/v1/foo
  [--with-test] [--patch-openapi]`
- Background job: `./scripts/new-job.sh <type>`
- Feature module (config flag + `Core::<name>_enabled()` + compose/helm/docs
  wiring): `./scripts/new-module.sh <name>`
- Migration: `make new-migration SLUG=<slug>`
- React page: `./scripts/new-react-page.sh`
- Changelog entry: drop a fragment `changelog.d/<topic>.<type>.md` (type ∈
  added|changed|fixed|removed|security; bullet text WITHOUT the leading
  `- `, format: `changelog.d/README.md`) — parallel-safe, two PRs never
  conflict; editing `[Unreleased]` directly stays legal.
  `./scripts/assemble-changelog.sh --check` gates fragment FORMAT in CI
  (fragments themselves are optional).
- Release: `./scripts/release.sh <ver>` — folds `changelog.d/` fragments
  into `[Unreleased]` (assemble-changelog.sh), retitles it to
  `## [<ver>] — date` with a fresh empty `[Unreleased]` on top (a heading
  retitled by hand is accepted too), then bumps every other version point
  in one command (CMakeLists + 9 helm tag pins + 4 Chart.yaml appVersions +
  `.template-version`); commits nothing (CONTRIBUTING.md "Release")
- Fork→template sync: `./scripts/sync-upstream.sh` — run IN a fork; pulls
  template fixes by three-way patching between release tarballs (base =
  `.template-version` stamp), so degit forks without shared git history get
  conflict markers instead of silent overwrites. In a fork, `project.env`
  `TEMPLATE_FORK=1` makes release.sh/check-version-sync leave the stamp to
  the sync script (docs/UPSTREAM.md).

The generators encode the invariants below — their output passes the CI
gates by construction. Hand-rolled versions usually don't.

## Invariants the CI gates enforce

1. **Route triple-sync:** every `ADD_METHOD_TO` in a controller must also
   appear in `Api::get_endpoints()` (`src/api/Endpoints.hpp`) **and** in
   `docs/openapi.yaml`. `scripts/check-openapi-drift.sh` and
   `scripts/check-routes-registered.sh` fail CI on any mismatch. Response
   BODIES are checked too: the e2e bucket validates real responses against
   the spec's schemas (`tests/e2e/openapi_check.hpp`, subset validator).
   It reads `tests/e2e/openapi.gen.json` — a committed conversion of
   `docs/openapi.yaml`; after editing the spec run
   `./scripts/gen-openapi-json.sh` and commit both, or the e2e test
   `OpenApiSpec.GenJsonIsFreshAndLoadable` fails on the stale hash stamp.
2. **API versioning (ADR 0006):** business routes live under `/api/v1`;
   `new-endpoint.sh` rejects unversioned paths. Probe routes (`/healthz`,
   `/ready`, `/health`, `/metrics`) stay unversioned.
3. **Source layout (ADR 0003, amended 2026-08-22):** leaf utilities, domain
   structs and ALL template code stay header-only in `.hpp`; modules past
   the `scripts/bench-incremental.sh` threshold get their non-template
   bodies de-inlined into a paired `.cpp` compiled ONCE into the `app_core`
   STATIC library (CMake picks up any `src/**/*.cpp` via GLOB — no CMake
   edit needed; billing is de-inlined, more modules follow by measured
   weight). Drogon route macros (`ADD_METHOD_TO`) always stay in the
   controller `.hpp` — the route gates grep only headers.
4. **One error shape:** `{error, status, message, ...}` everywhere — use
   `ErrorResponse::*` / `Api::Validation::*`, never hand-rolled error JSON.
5. **Test buckets by directory** (`scripts/check-test-buckets.sh`):
   `tests/unit` (no services), `tests/integration` (real Postgres/Redis),
   `tests/api` (controller via `TestHelpers::make_request`), `tests/e2e`
   (real HTTP server, separate binary).
6. **Migrations:** `migrations/NNN_slug.sql`, sequential numbering, no
   `BEGIN`/`COMMIT` (the runner wraps them; use the
   `-- migrate:no-transaction` marker for `CREATE INDEX CONCURRENTLY`).
7. **No secrets in tracked files:** `config/config.json` holds `${VAR}`
   placeholders, env overrides everything (`docs/CONFIG.md` is the full
   table). gitleaks gates CI; `make prod-check` gates the prod profile.
   Helm credential defaults stay EMPTY (deploys pass them via `--set`;
   `scripts/check-helm-render.sh` asserts no tracked overlay renders a
   non-empty credential Secret). In `.gitleaks.toml` the `[extend]
   useDefault` block is load-bearing — without it gitleaks v8 scans with
   zero rules; allowlist by VALUE regex, never by path.
8. **Module dependency DAG** (`scripts/check-module-deps.sh`): every
   cross-directory `#include` in `src/` must be an edge declared in
   `docs/module-deps.txt`. Hard rules: `utils/` includes only `utils/`;
   `core/Core.hpp` (the composition root) is included ONLY by
   `src/main.cpp`, `src/worker_main.cpp`, `src/api/HealthController.hpp`
   and its own body file `src/core/Core.cpp`
   — for `Core::<module>_enabled()` / `Core::is_shutting_down()` include
   the tiny `core/Modules.hpp`; `webhooks` never includes `email` (shared
   curl bootstrap lives in `utils/CurlInit.hpp`). `api/Api.hpp` is included
   only by binary entry points (main.cpp, tests/e2e) — tests include the
   specific controller header they exercise.
9. **Config key sync** (`scripts/check-config-sync.sh`): every
   `cfg.get<T>("json.path", "ENV", default)` read in `src/` must have its
   json path in `config/config.json` AND `config/config.sample.json` (as a
   `${ENV:-default}` placeholder matching the code default) and its ENV
   documented in `docs/CONFIG.md`; stale doc rows and helm env lines for
   envs the code no longer reads also fail. Regex-invisible reads get a
   commented entry in `docs/config-sync-allowlist.txt` (never a silent
   skip). NB: `get<std::string>` returns a present-but-empty json value
   AS-IS — don't add an empty-expanding placeholder for a string key whose
   code default is non-empty (see `rate_limit.protected_paths` there).
10. **Commits:** conventional commits, no AI-attribution trailers.

## Gate sequence — run cheapest-first before pushing

1. `make fmt` — clang-format in place (refuses to run unless clang-format
   is major 17, the CI pin; fix: `pip install clang-format==17.0.6`)
2. `./scripts/check-openapi-drift.sh && ./scripts/check-routes-registered.sh
   && ./scripts/check-test-buckets.sh && ./scripts/check-version-sync.sh
   && ./scripts/check-frontend-nginx-sync.sh && ./scripts/check-module-deps.sh
   && ./scripts/check-config-sync.sh && ./scripts/assemble-changelog.sh --check`
   — seconds, no build.
   Touched a `check-*` script (or assemble-changelog.sh)? Also run
   `./scripts/check-selftest.sh` —
   plants 20 breakages and requires every gate to catch and name them
   (needs helm+yq; in CI `gate-selftest` self-scopes to diffs touching
   `scripts/`, `helm/` or `.github/workflows/`, with a nightly
   unconditional backstop in `.github/workflows/gates-nightly.yml`)
3. `make lint-openapi` — spectral over `docs/openapi.yaml`
4. `make test` — rebuild (docker layer cache) + full suite, ~2 min warm;
   what CI runs. `make test-quick` is an honest alias for it. `make
   test-rerun` re-runs the previous binaries WITHOUT rebuilding — flake
   triage only, code edits do NOT land in it. Fastest inner loop for code
   changes: native `make test-local NAME='Foo*'` (docs/TESTING.md)
5. `make helm-lint` — only if `helm/` was touched
6. `make ci-local` — full local reproduction of CI

CI additionally runs clang-tidy, ASan+UBSan (+TSAN) over the unit AND
integration/api buckets (integration against real compose-profile
Postgres/Redis, CI_REQUIRE_INFRA=1), gitleaks, helm-render,
the OpenAPI-drift gate and the gate selftest; C++ compiles in CI go through sccache backed by
the Actions cache. Trivy scans images in the release pipeline
(`.github/workflows/release.yml`), not in per-PR CI. The heavy jobs
(build-and-test, clang-tidy, sanitizers, tsan, runtime-smoke, frontend,
gate-selftest) SELF-scope: they always start (required-check semantics stay
honest — never a `paths:` filter), diff the change set themselves, and exit
green in seconds when their input paths are untouched (docs/CI-PROFILES.md
explains the mechanism and the path lists).

Release supply chain: release.yml also generates SPDX SBOMs (syft), signs
all three images keyless with cosign by digest and attests the SBOMs
(verify commands: SECURITY.md). The swagger-ui-dist CDN pin in
`src/api/Middleware.cpp` is Renovate-tracked (regex manager, never
automerged — bumps need both sha384 SRI hashes re-pinned by hand, recipe in
the comment at the pin); the nightly `swagger-sri-guard` job in
gates-nightly.yml re-downloads the pinned files and fails on pin/hash drift.
`fuzz-nightly.yml` runs libFuzzer over the byte-facing parsers
(`tests/fuzz/`, standalone Clang build — no vcpkg/app_core; keep those
parser TUs std-only, see docs/TESTING.md "Fuzzing").

An opt-in rendered-artifact gate for forks that render documents ships as
`scripts/check-artifact.py` + `scripts/render-artifacts.sh` + a mandatory
selftest (`scripts/check-artifact-selftest.sh`) — not wired into this repo's
CI; the fork enables it per `docs/RENDER-GATE.md`.

## Don'ts

- Don't edit the `builtin-baseline` in `vcpkg.json` or `ARG VCPKG_REF` in
  `docker/Dockerfile` by hand — Renovate owns them, and a baseline bump
  rebuilds the entire dependency world.
- Don't weaken `config/config.production.json` — `make prod-check` gates it.
- Don't bump `version` in `vcpkg.json` — the Dockerfile keys the whole
  dependency-install layer on that manifest, so touching it rebuilds ~29
  packages and blows CI timeouts. The release version lives in
  `CMakeLists.txt` `project(VERSION …)`, the helm image-tag pins, the
  `Chart.yaml` appVersions and the `.template-version` stamp (template repo
  only — a fork's stamp is owned by `sync-upstream.sh`), and must match the
  newest CHANGELOG heading
  (`scripts/check-version-sync.sh` gates all of them; `scripts/release.sh`
  bumps all of them).
- Don't change the error-response shape without updating `docs/openapi.yaml`.
- Don't accumulate with a self-referencing upsert through
  `Database::execute_write` — `INSERT ... ON CONFLICT DO UPDATE SET x = t.x +
  EXCLUDED.x` has computed as though the existing row were absent (every
  second-or-later write counted from zero; root cause never found — forensics
  in the site fork's commit b676430). Pattern instead: `INSERT ... ON CONFLICT
  DO NOTHING` → `SELECT ... FOR UPDATE` → compute the new value in C++ →
  plain `UPDATE` — canonical in-repo example: the wallet ledger in
  `src/billing/Wallet.hpp`.
- Don't use inja's default `{#`/`#}` comment markers in templates that carry
  TeX-like content — a `#1`-style macro parameter (`{#1}`) opens an inja
  comment that never closes and the whole render dies with a parser error at
  EOF. Remap via `Environment::set_comment` (the cyber-accountant fork uses
  `((#`/`#))`).
- Don't open a new auth-public route by editing `api.public_paths` (it's a
  FULL override — re-listing the default set is how routes get silently
  dropped) — add it to the additive `api.public_paths_extra` /
  `API_PUBLIC_PATHS_EXTRA` instead.

## Self-maintenance

When a PR adds or changes a CI gate, scaffolding script, or invariant,
update this file in the same PR.
