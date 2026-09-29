# CI profiles: hosted runners, self-hosted runners, and why there is no arm64 CI

The workflows in `.github/workflows/` are tuned for GitHub-hosted runners.
This page records what changes (and what breaks) when you move them to
self-hosted/weak runners, and why arm64 is deliberately out of CI. Every rule
here was learned the expensive way on downstream forks of this template.

## Hosted-runner profile (the default in this repo)

- **Heavy jobs run in parallel.** `build-and-test`, `clang-tidy`, `sanitizers`
  and `tsan` each get their own VM, so parallelism costs nothing; each pulls
  builder layers from the gha/GHCR caches independently. Serializing them on
  hosted runners only stretches the wall clock.
- **Heavy jobs self-scope; cheap jobs stay unconditional.** The expensive
  jobs (`build-and-test`, `clang-tidy`, `sanitizers`, `tsan`,
  `runtime-smoke`, plus `frontend` and `gate-selftest`) each begin with a
  `scope` step: the job always STARTS, lists the changed files itself
  (`gh api .../pulls/N/files` on PRs; `git diff HEAD^..HEAD` with
  `fetch-depth: 2` on pushes), and if none of its declared input paths are
  touched it exits green in seconds with an explicit "paths untouched —
  verified nothing is affected" log line. This is NOT the "skipped job goes
  green" trap: a workflow-level `paths:` filter makes GitHub skip the
  required job entirely and report success without running a single
  instruction, while a self-scoped job actually executed, actually computed
  the diff, and actually verified its irrelevance — the green check is a
  real (cheap) verification, and any failure to compute the diff fails OPEN
  into a full run. Measured motivation: one docs typo used to cost 5 Docker
  image builds and 30+ CPU-minutes. Every scope list includes
  `.github/workflows/ci.yml` itself, so edits to the scoping logic can
  never scope themselves out. The genuinely cheap jobs (`lint-format`,
  `secret-scan`, `openapi-drift`, `helm-charts`) stay unconditional — they
  cost seconds and scoping them would buy nothing.
- **`gate-selftest` is self-scoped + nightly.** Per-PR it runs
  `scripts/check-selftest.sh` only when the diff touches `scripts/`,
  `.github/workflows/` or `helm/` (where gate mutations live); the daily
  unconditional backstop is `.github/workflows/gates-nightly.yml`
  (cron + `workflow_dispatch`: check-selftest.sh + check-module-deps.sh),
  so gate rot is caught within a day even when nobody touches the gates.
- **Three cache layers.** `cache-from` lists `type=gha` plus
  `ghcr.io/<repo>/builder:cache`. The gha cache is evicted aggressively
  (7 days / 10 GB); the GHCR layer survives, EXCEPT after a vcpkg baseline
  bump, when the dependency world legitimately rebuilds (~35 min). On top of
  the image-layer caches, compilation itself goes through **sccache**
  (v0.17.0, pinned in `docker/Dockerfile`) backed by the GitHub Actions
  cache: the Actions credentials (`ACTIONS_RESULTS_URL` /
  `ACTIONS_RUNTIME_TOKEN`, exposed by `crazy-max/ghaction-github-runtime`)
  reach the Docker build as BuildKit **secrets** — never ARGs — so layer
  cache keys and image history stay credential-free, and
  `ACTIONS_CACHE_SERVICE_V2=on` is mandatory (without it sccache's ghac
  backend silently caches nothing). With 100% sccache hits the builder-stage
  compile takes ~24 s; warm wall clocks are roughly build-and-test ~12 min,
  clang-tidy ~4, runtime-smoke ~2.5. `sanitizers` and `tsan` were ~4 when
  they covered the unit bucket only; since they also build and run the
  integration/api bucket against the compose `test`-profile Postgres/Redis
  sidecars (with `CI_REQUIRE_INFRA=1` so missing infra fails instead of
  skipping green), budget extra minutes for the second binary's compile plus
  the instrumented suite run — more under TSan than ASan.
- **Timeouts must let one cold build finish.** A job killed by its timeout
  BEFORE it exports the gha cache leaves the next run just as cold — the
  short timeout then reproduces the slow run it was guarding against. That is
  why the heavy compile jobs carry generous timeouts: build-and-test
  `timeout-minutes: 90`, sanitizers and tsan `120` (cold vcpkg world + the
  slower instrumented compile + the instrumented integration run),
  clang-tidy 90.

## Self-hosted / weak-runner profile

If you point these workflows at self-hosted runners (ARC on Kubernetes, a
single beefy box, etc.), apply all of the following (each was a real incident
on a downstream fork):

1. **Serialize the heavy jobs.** Concurrent cold builds starve a shared
   runner: with build-and-test, clang-tidy and sanitizers all compiling the
   vcpkg world at once under a tight cap, every job gets killed before ANY of
   them exports a cache layer, so every subsequent run is cold too. Chain
   them (`clang-tidy`/`sanitizers`: `needs: build-and-test`) so the first
   job warms the cache once and the rest reuse it.
2. **Set honest timeouts.** Budget for the worst realistic case (cold vcpkg
   world + full suite), not the happy path. A generous timeout on a green run
   costs nothing; a tight one on a cold run costs the cache export and hence
   every following run.
3. **Retry transient fetch failures.** vcpkg downloads tarballs from GitHub,
   which intermittently answers 400/429, and vcpkg refuses to retry by
   itself. This repo retries inside `docker/Dockerfile` (clone + install,
   3 attempts; the cache mounts make retries cheap). On CI systems with
   job-level retries (GitLab `retry:`), also add
   `max: 2, when: [runner_system_failure, stuck_or_timeout_failure,
   script_failure]` to the heavy jobs.
4. **Don't assume tools exist on the runner image.** Minimal ARC images ship
   without `make` — compile steps go green and then the `make test` step dies
   with exit 127. Install the small tools (`make`, `jq`, …) in an early step
   or bake them into the runner image.

## arm64: build on real hardware or not at all

A C++ build of this dependency world under QEMU emulation (buildx
`linux/arm64` on an amd64 hosted runner) takes HOURS, not minutes — it never
fit any CI budget and was removed from the release workflow; release images
are amd64-only by default. If you need arm64 images, either build on native
arm64 hardware (a self-hosted arm64 runner) or accept amd64-only. Do not
re-add a QEMU arm64 leg to CI "for completeness" — it will time out.

When you do build multi-arch on native hardware, keep the
`id=vcpkg-downloads-${TARGETARCH}` scoping on the BuildKit cache mounts in
`docker/Dockerfile`: both platform legs run concurrently, and an unscoped
cache mount is shared between them — vcpkg then extracts its tools (ninja)
into the same path from both legs and one leg executes the other's binary.
