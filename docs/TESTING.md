# Testing

What the test suite does and does not cover, and how to run each part. The goal
is that "the suite is green" means something specific — not "451 passed, 171 of
them skipped."

## Buckets

| Bucket | Count | Needs | Runs with | What it covers |
|---|---:|---|---|---|
| **unit** | 265 | nothing (sidecar-free) | `make test-unit` | Pure logic: validation, tokens, JWT, password hashing, rate-limit math, serialization, the permission bitmask, retry/backoff, templates. |
| **integration** | 151 | Postgres + Redis | `make test` | Repositories against a real Postgres, cache + rate limiter against a real Redis, migrations, the account/admin/audit/auth flows, job dispatch + DLQ. |
| **api** | 20 | Postgres + Redis | `make test` | Controller request/response behavior wired through the real handler stack. |
| **e2e** | 15 | Postgres + Redis | `make test-e2e` | A real Drogon server + client on the wire: auth gate, cookie sessions, refresh rotation/revocation, Idempotency-Key replay, tracing headers. |

Total: **451** test cases. `make test-unit` is the fast, dependency-free loop;
`make test` brings up sidecars and runs unit + integration + api, then the e2e
binary; `make test-e2e` runs just the wire-level suite. `make ci-local` runs
the lot the way CI does.

## Day-to-day loops — which target when

- **Recommended inner loop: `make test-local NAME='Foo*'`** — native
  incremental build (CMake `dev` preset, no Docker) + both gtest binaries with
  a `--gtest_filter`. Seconds per iteration once configured. One-time setup:
  set `VCPKG_ROOT` and run `make configure-local` — or skip setup entirely and
  open the dev container (next section). The integration binary
  needs a running stack (`make up`); without it those suites skip locally.
  Variants: `make test-unit-local` (sidecar-free subset), `make test-watch`
  (re-run on save via watchexec/entr).
- **`make test`** (alias: `make test-quick`) — rebuild the Docker test image
  with the layer cache, then the full suite. Warm builder layers: ~2 min,
  dominated by actual compilation of your change — that time is the price of
  the run testing the code you wrote. Cold (no `make warm-cache`): ~30 min.
  This is what CI runs.
- **`make test-rerun`** — re-run the LAST-BUILT test image without
  rebuilding. Source edits do **not** land in it, so it can never verify a
  change; its only honest use is re-checking a flaky test. (This was the old
  `test-quick` behavior — renamed because "~5 s green" after an edit proved
  nothing.)

## Dev container — the inner loop with zero setup

`.devcontainer/` opens the repo inside the CI-published builder image
(`ghcr.io/your-registry/your-project/mi-fitness-api/builder:cache`): compiler, cmake,
ninja and the entire prebuilt vcpkg world are already in it, so the first
configure is ~15 s and `make test-local NAME='Foo*'` works immediately — no
`VCPKG_ROOT`, no ~30-minute cold dependency build (the container preselects
`PRESET=devcontainer`, whose preset points the toolchain at the image's
`/app/build/vcpkg_installed`). The upstream package is public; if your fork's
GHCR package is private, `docker login ghcr.io` before opening. Docker-in-docker
is included, so `make up` and the full `make test` also run inside — but the
recommended inner loop stays `test-local`. The image is linux/amd64 only: on
Apple Silicon it runs under emulation (works, but compiles are several times
slower — prefer an x86_64 host or a codespace). Build parallelism is bounded by
RAM (`JOBS` in the Makefile): heavy test TUs peak ~1.5-2 GiB each, and an
unbounded `-j` OOM-kills cc1plus on an 8 GiB VM.

## Coverage

`make coverage` builds with instrumentation and runs **all** buckets, so the
number reflects the DB/cache/auth/jobs code too — not just unit-reachable lines.
The integration and e2e buckets need Postgres + Redis (`make up` first); without
them those buckets are skipped and the reported coverage drops accordingly.

## Fuzzing

The parsers that eat **external bytes** have libFuzzer harnesses in
`tests/fuzz/`, run nightly by `.github/workflows/fuzz-nightly.yml`
(cron + `workflow_dispatch`, ~120 s per target):

| Target | Parser under test | External input it models |
|---|---|---|
| `fuzz_traceparent` | `Observability::Trace::parse_traceparent` | `traceparent` header, straight off the network on every request |
| `fuzz_decimal_cents` | `Billing::detail::parse_decimal_to_cents` (+ round-trip via `cents_to_decimal_string`) | amount strings in PayPal API/webhook bodies |
| `fuzz_config_expand` | `Config::detail::expand_string` / `substitute_env_placeholders` | `${VAR}` placeholders in config files (fixed, cleared env for determinism) |
| `fuzz_path_match` | `Utils::Strings::path_is_public` + CSV split/merge, `Api::normalize_path_for_metrics` | request paths and public-path CSV config |

Harnesses check invariants, not just "no crash": accepted traceparents must be
canonical and format→parse round-trip; accepted amounts must be non-negative
and survive a cents→string→cents round trip; path normalization must stay
rooted and idempotent.

**Toolchain decision (why a separate build dir).** libFuzzer requires Clang;
CI's production toolchain is GCC + vcpkg, and rebuilding the vcpkg dependency
world under a second compiler is a non-starter for a nightly. So the fuzzers
deliberately do **not** link `app_core`: each harness compiles the specific
parser TU directly, and those TUs are kept std-only for exactly this reason
(`Trace.cpp`, `billing/PayPalParse.cpp`, `utils/Strings.cpp`;
`utils/ConfigExpand.cpp` additionally needs header-only nlohmann/json, taken
from the distro package). If you move a fuzzed parser into a TU with heavier
includes, the fuzz build breaks — that's the tripwire telling you to keep the
byte-facing surface dependency-free.

Run locally (needs clang; never the normal build dir):

```bash
cmake -S tests/fuzz -B build-fuzz -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz -j
./build-fuzz/fuzz_traceparent tests/fuzz/corpus/fuzz_traceparent   # + libFuzzer flags
```

Seed corpora live in `tests/fuzz/corpus/<target>/` (valid + boundary inputs).
The nightly job carries a **growing** corpus between runs via the Actions
cache and treats the seeds as a read-only starting set. A crash fails the job
and uploads the reproducer input as the `fuzz-reproducers` artifact; triage by
re-running `./build-fuzz/<target> <reproducer-file>`.

Not fuzzed (documented limitation): everything behind the network seam —
`verify_webhook_signature` beyond its pure header/JSON handling
(`find_header_ci` is compiled into the decimal target's TU but the
curl-touching flow is not), `parse_capture_response` (nlohmann-based, lives in
the curl TU), and anything needing Drogon types.

## Known gaps (be honest about these before you rely on them)

- **No behavioral coverage** for Kafka messaging, SMTP delivery (the Mailer is
  exercised through the jobs path, not against a real SMTP server), or Postgres
  streaming replication. These have lifecycle/health guards only — wiring, not
  behavior.
- **Frontend** has unit tests for the session-refresh machinery and the
  permission mirror, but no component/route tests for the admin/auth UI.
- **Sanitizers (ASan/UBSan and TSan)** cover the **unit** and
  **integration/api** buckets: the CI `sanitizers` and `tsan` jobs build both
  test binaries and run the integration one against the compose `test`-profile
  Postgres/Redis with `CI_REQUIRE_INFRA=1` (a missing sidecar fails the run
  instead of skipping it green). The **e2e** binary is still uninstrumented.
  Historical note: integration was unit-only for a while — compiling its TUs
  under ASan OOM'd an 8 GB build VM back when every heavy body was header-only;
  the `app_core` STATIC extraction (ADR 0003 as amended) compiles those bodies
  once and removed the blocker.

## A disabled test that marks a real bug

`tests/integration/test_jobs.cpp` contains
`DISABLED_CancelIsAtomicUnderContention`. It is **disabled because it documents
an unfixed race**, not because it is flaky: two concurrent callers of a job's
`cancel()` can both observe the not-yet-cancelled state and both write a terminal
status (a TOCTOU on the job row). Re-enable it once `cancel()` does a single
conditional state transition (e.g. an `UPDATE ... WHERE status = 'pending'`
guard) instead of read-then-write. Until then, treat single-cancel as supported
and concurrent-cancel as undefined.
