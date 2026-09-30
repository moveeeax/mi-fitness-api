# Documentation index

One-line navigator across every doc, ADR, and config in the repo. Use this
as the entry point when you need to find the right file for a
question instead of grepping the tree.

## Top-level guides

| File | What's there |
|---|---|
| [`../README.md`](../README.md) | Getting started, what's in the box, quickstart, repo layout |
| [`../CONTRIBUTING.md`](../CONTRIBUTING.md) | Pre-commit setup, dev workflow, commit-message convention, release flow |
| [`../SECURITY.md`](../SECURITY.md) | Disclosure policy + production-hardening checklist |
| [`../CHANGELOG.md`](../CHANGELOG.md) | Versioned change log (semver; authored via `changelog.d/` fragments — parallel-safe, see [`../changelog.d/README.md`](../changelog.d/README.md) — or directly under `## [Unreleased]`) |
| [`../REMOVING-THE-DEMO.md`](../REMOVING-THE-DEMO.md) | What's reference-only (flask-base) vs the real app, and how to strip it (`init-project.sh --no-demo` / `--minimal`, `remove-content-module.sh`) |

## Worked examples & deep-dives

| File | What's there |
|---|---|
| [`EXAMPLES.md`](EXAMPLES.md) | End-to-end CRUD walkthrough for adding your own resource: migration → DTO → repository → controller → tests |
| [`CONVENTIONS.md`](CONVENTIONS.md) | Canonical "add a domain entity" checklist (what `new-resource.sh` follows) + what NOT to abstract |
| [`CONFIG.md`](CONFIG.md) | Single table mapping every JSON key ↔ env var ↔ default |
| [`TESTING.md`](TESTING.md) | Test buckets (unit/integration/api/e2e), day-to-day loops incl. the `.devcontainer` zero-setup inner loop, coverage, nightly libFuzzer targets (`tests/fuzz/`), the disabled-race note |
| [`ORGS.md`](ORGS.md) | Multi-tenancy starter kit (`scripts/add-orgs.sh`): two role layers, fail-closed org context, claim/switch semantics, deny-by-default matrix |
| [`UPSTREAM.md`](UPSTREAM.md) | Fork↔template sync: `scripts/sync-upstream.sh` (three-way tarball patching for degit forks, `.template-version` stamp), git merge for full-history forks, the backport-candidate discipline for giving generic fixes back |
| [`BENCHMARKS.md`](BENCHMARKS.md) | How to measure latency/throughput/footprint (`make bench` + presets) + a results template |
| [`PATTERNS-FROM-FLASK-BASE.md`](PATTERNS-FROM-FLASK-BASE.md) | Authoritative list of patterns lifted from flask-base (file-level mapping included) |
| [`openapi.yaml`](openapi.yaml) | OpenAPI 3.1 spec for every registered route. `scripts/check-openapi-drift.sh` keeps it honest; `frontend/npm run gen:api` consumes it for typed client |
| [`module-deps.txt`](module-deps.txt) | Allowed include edges between `src/` directories — the declared architecture `scripts/check-module-deps.sh` enforces (utils is the bottom layer; `core/Core.hpp` only from binary entry points; no `webhooks -> email`) |
| [`config-sync-allowlist.txt`](config-sync-allowlist.txt) | Commented exceptions for `scripts/check-config-sync.sh`: config reads the extractor regex can't see, and keys deliberately absent from the config jsons |
| [`Doxyfile`](Doxyfile) | `make docs` configuration; output goes to `docs/html/` (gitignored) |
| [`superpowers/`](superpowers/) | Design specs + implementation plans: the ACTIVE modularity target design (`specs/2026-08-22-modularity-design.md`) and archived past waves (content module, bench nightly, hygiene/hardening) |

## Frontend

| File | What's there |
|---|---|

## Architecture decision records (`adr/`)

| ADR | Decision |
|---|---|
| [`adr/0001-drogon-http-framework.md`](adr/0001-drogon-http-framework.md) | Why Drogon over Crow / Pistache / cpp-httplib |
| [`adr/0002-nlohmann-json.md`](adr/0002-nlohmann-json.md) | nlohmann::json end-to-end (Drogon's jsoncpp is internal-only) |
| [`adr/0003-header-only-modules.md`](adr/0003-header-only-modules.md) | All `src/` modules are `.hpp`; only `main.cpp`/`worker_main.cpp` are TUs |
| [`adr/0004-global-singletons.md`](adr/0004-global-singletons.md) | Module init/get/shutdown singleton pattern + ordering rationale |
| [`adr/0005-spa-split.md`](adr/0005-spa-split.md) | Frontend is a separate React SPA (not SSR), deployable on its own |
| [`adr/0006-api-versioning.md`](adr/0006-api-versioning.md) | URL-path `/api/v1` versioning with additive-only evolution (probe routes stay unversioned) |
| [`adr/README.md`](adr/README.md) | ADR conventions + how to add a new one |

## Migrations

| File | What's there |
|---|---|
| [`../migrations/README.md`](../migrations/README.md) | Migration conventions, NNN_*.sql naming, runner behaviour, seed flow |
| [`../scripts/new-migration.sh`](../scripts/new-migration.sh) | Generate the next numbered migration (NO BEGIN/COMMIT — the runner wraps each file in one advisory-locked transaction) |

## Build & test

| File | What's there |
|---|---|
| [`../CMakeLists.txt`](../CMakeLists.txt) | Build graph, common libs, ASan/coverage/Werror options |
| [`../CMakePresets.json`](../CMakePresets.json) | `dev`, `dev-asan`, `release`, `coverage` presets (vcpkg toolchain) |
| [`../vcpkg.json`](../vcpkg.json) | Manifest-mode dependency list with pinned baseline |
| [`../Makefile`](../Makefile) | Single entry point — `make help` lists every target |
| [`../envrc.sample`](../envrc.sample) | Sample direnv config for native build (VCPKG_ROOT, TEST_PG_HOST, etc.) |

## CI / Ops

| File | What's there |
|---|---|
| [`../.github/workflows/ci.yml`](../.github/workflows/ci.yml) | GitHub Actions CI: build + test + format + secret-scan |
| [`CI-PROFILES.md`](CI-PROFILES.md) | Hosted vs self-hosted runner profiles: serialize-vs-parallel, cache layers, honest timeouts, retry policy, why arm64/QEMU is out |
| [`../.github/workflows/release.yml`](../.github/workflows/release.yml) | Tag-driven image build → Trivy gate → promote → SPDX SBOM + cosign keyless sign/attest → GitHub Release (verify: [`../SECURITY.md`](../SECURITY.md)) |
| [`RENDER-GATE.md`](RENDER-GATE.md) | Opt-in rendered-artifact gate for forks that render documents (Typst/LaTeX/HTML→PDF): contract, provenance rule, selftest, example CI job — not wired into this repo's CI |
| [`RUNBOOK.md`](RUNBOOK.md) | Operator runbook: what to do when an alert fires (each alert's `runbook_url` anchors here) |
| [`SLO.md`](SLO.md) | SLOs + alert thresholds — the rationale behind the Prometheus rules |
| [`../helm/mi-fitness-api/`](../helm/mi-fitness-api/) | API Helm chart |
| [`../helm/mi-fitness-api-worker/`](../helm/mi-fitness-api-worker/) | Worker Helm chart |

## Scripts (`scripts/`)

| Script | Purpose |
|---|---|
| `init-project.sh` | One-shot rename of template identity (project name, registry, helm charts); `--no-demo` / `--minimal` strip demo weight, `--with-orgs` chains `add-orgs.sh` |
| `remove-content-module.sh` | ONE-SHOT removal of the content module (posts/uploads/sitemap) with all sync gates kept green — see REMOVING-THE-DEMO.md |
| `new-resource.sh` | Scaffold a FULL CRUD resource (domain + repository + controller + registry + openapi + test) per docs/CONVENTIONS.md; `--owned` per-user, `--org-scoped` per-tenant |
| `add-orgs.sh` | ONE-SHOT installer of the multi-tenancy starter kit (src/tenancy/, org guards, orgs API, migration, tests) — see docs/ORGS.md |
| `new-endpoint.sh` | Scaffold a single controller + registry row + optional test + optional OpenAPI patch |
| `new-job.sh` | Scaffold a background-job handler that self-registers with the dispatcher (one `#include` to wire into the worker) |
| `new-module.sh` | Scaffold a feature-module master switch (config flag + `Core::<name>_enabled()` + compose/helm/docs wiring), following the content module's on/off pattern |
| `new-migration.sh` | Generate the next `NNN_<slug>.sql` |
| `check-openapi-drift.sh` | Verify `Api::get_endpoints()` (src/api/Endpoints.hpp) ↔ `docs/openapi.yaml` (method, path) |
| `check-routes-registered.sh` | Verify every controller ADD_METHOD_TO route is in `Api::get_endpoints()` (symmetric to the OpenAPI drift check) |
| `check-test-buckets.sh` | Verify test suites sit in the right bucket — classified by DIRECTORY, fails on a suite-name clash across unit/integration |
| `release.sh` | One-command release: folds `changelog.d/` fragments into `[Unreleased]`, retitles it to `## [<ver>] — date` (accepts a hand-retitled heading too), bumps every version point (CMakeLists + 9 helm tag pins + 4 appVersions); verifies with check-version-sync.sh, commits nothing |
| `assemble-changelog.sh` | Fold `changelog.d/<topic>.<type>.md` fragments into CHANGELOG's `[Unreleased]` (creates `###` sections in Keep-a-Changelog order, deletes consumed fragments); `--check` validates fragment format only — CI runs it, fragments themselves stay optional |
| `check-module-deps.sh` | Verify every cross-directory `#include` in `src/` is an edge declared in `docs/module-deps.txt`; hard-forbids `utils -> *`, non-entry-point includes of `core/Core.hpp`, and `webhooks -> email` |
| `check-config-sync.sh` | Verify every config read in `src/` (`cfg.get<T>("path", "ENV", …)` + getenv) exists in `config/config.json` AND `config/config.sample.json` and is documented in `docs/CONFIG.md`; flags stale doc rows and stale helm env lines (exceptions: `docs/config-sync-allowlist.txt`) |
| `check-helm-render.sh` | Render the cpp-env umbrella with CI values and assert deploy-path invariants (ports, hosts, empty credential defaults) |
| `check-artifact.py` | Content + leaked-template-syntax gate over ONE rendered artifact's extracted text (opt-in, for document-rendering forks — see docs/RENDER-GATE.md) |
| `render-artifacts.sh` | Loop for the opt-in artifact gate: render every `templates/render/*/fixtures/*.json` via `RENDER_CMD`/`EXTRACT_CMD`, then gate with `check-artifact.py`; zero fixtures = failure |
| `check-artifact-selftest.sh` | Mandatory selftest for the artifact gate: healthy example (incl. hostile data fixture) must PASS, two counted mutations must be caught and named |
| `check-selftest.sh` | Prove all nine gate scripts bite (the eight `check-*` gates + `assemble-changelog.sh --check`): plant 20 known breakages in scratch copies, require each gate to fail AND name it (control-pass first, counted mutators, a no-op mutation is itself a failure; needs helm+yq) |
| `prod-check.sh` | Pre-deploy assertions on a production config (auth, secrets, TLS, fail-closed) |
| `lint-openapi.sh` | Spectral lint with project ruleset |
| `make-jwt.sh` | Mint a dev HS256 JWT (no Python/Node deps) |
| `smoke.sh` | curl through critical endpoints (health, traceparent, validation, metrics) |
| `bench.sh` | wrk benchmark with config presets |
| `bench-ci.sh` | CI/nightly benchmark orchestrator — emits the JSON github-action-benchmark reads (`wrk2bench.sh` converts wrk output; `bench-incremental.sh` measures warm rebuild times) |
| `deploy-demo.sh` | Deploy / update the public demo environment (cpp-env umbrella) |
| `env-check.sh` | Report unset `${VAR}` placeholders in config without defaults |
