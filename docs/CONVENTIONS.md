# Conventions — how to add a domain entity

This is the canonical, copy-this checklist for adding a new resource end to
end. It points at the real files that already implement `users` / `roles` /
`posts`, so you extend an existing pattern instead of guessing which
controller to mimic.

> Fast path: `./scripts/new-resource.sh Product` scaffolds the backend stubs
> (domain + repository + CRUD controller + endpoint registry + openapi block +
> test) following every rule below. Then fill in the fields and the frontend.

The friction this guards against: one field name lives in ~6 places (SQL,
`from_row`, `to_json`, the openapi schema, the generated TS types, the page),
and several of those drift **silently** — the build stays green. Follow the
order below and run the drift checkers.

---

## 1. Migration — `migrations/NNN_<slug>.sql`

`./scripts/new-migration.sh add_products` writes a numbered, idempotent
skeleton. Inside: `CREATE TABLE IF NOT EXISTS`, indexes, and an
`updated_at` trigger attached to the shared `touch_updated_at()` function
from `migrations/000_updated_at_trigger.sql` (`CREATE TRIGGER … EXECUTE
FUNCTION touch_updated_at()` — don't duplicate the function per table; see
`006_add_posts.sql`). Applied at boot in numeric order by
`MigrationRunner` under an advisory lock (safe with multiple replicas).

## 2. Domain DTO — `src/domain/<Entity>.hpp`

Model after `src/domain/User.hpp` / `Role.hpp`. Three things, and **every
field appears in all three** — keep them in sync:

- the `struct` fields (use `std::optional<T>` for nullable columns);
- `static <Entity> from_row(const Row&)` — `row["col"].template as<T>()`,
  guard nullable columns with `if (!row["col"].is_null())`;
- `inline void to_json(nlohmann::json&, const <Entity>&)` (ADL — `json j = e;`
  just works).

**Never serialize secrets** in `to_json` (see `User::password_hash`). The
invariant is enforced by `tests/unit/test_domain_serialization.cpp` — add a
case there if your entity has a sensitive field.

## 3. Repository — `src/repositories/<Entity>Repository.hpp`

Extend `CrudBase` (model after `RoleRepository`; `UserRepository` is the
hand-written variant because it joins roles):

- `class FooRepository : public CrudBase<FooRepository, Domain::Foo, std::string>`
  + four `static constexpr` constants (`kTable` / `kColumns` / `kIdColumn` /
  `kOrderBy`). CrudBase then supplies `find(id)` / `list(limit, offset)` /
  `count()` — don't re-hand-roll them. Hand-write only the bespoke writes.
- Typed exceptions deriving from the generic bases in
  `repositories/RepoErrors.hpp` — `struct FooNotFound : NotFoundError` (→404),
  `struct DuplicateFoo : ConflictError` (→409). They carry their own code, so
  `with_repo_errors` maps them without knowing the concrete type.
- Every write wraps `Database::get().execute_write([&](auto& txn){…})`. The
  lambda MUST take `auto& txn` (it receives `detail::TracingTxn&`, not a raw
  `pqxx::work&`). Use `execute_read_primary` for read-after-write.
- Two more `execute_*` variants exist for forks (nothing in the template
  itself calls them — don't let that fool you into deleting them):
  - `execute_write_idempotent(fn)` — same as `execute_write` but with the
    liberal read-style retry classifier, which may REPLAY the whole
    transaction after a connection-class error whose commit already landed.
    Safe only for writes keyed by a natural key (UPSERT by name, DELETE by
    PK) where a replay converges instead of double-applying.
  - `execute_transaction(IsolationLevel, fn)` — multi-statement transaction
    at `ReadCommitted` / `RepeatableRead` / `Serializable` (one-arg overload
    defaults to ReadCommitted). Retry classification matches
    `execute_write` (only PG-confirmed 40001/40P01 rollbacks — exactly what
    Serializable produces under contention).

  Both are pinned by real-Postgres tests in
  `tests/integration/test_database.cpp` and compile-checked against the DI
  seam in `tests/unit/test_database_seam.cpp`.
- Wrap UNIQUE/FK-tripping writes in `detail::translate_sql(...)`
  (`repositories/SqlErrors.hpp`) to turn a SQLSTATE into your typed exception —
  otherwise a constraint violation surfaces as a raw 500. For the ubiquitous
  single-SQLSTATE case use the `detail::throw_on` factory as the translator:
  `detail::throw_on<DuplicateFoo>("23505")` (see `UserRepository::create`).
- **Per-user resources:** add `static constexpr const char* kOwnerColumn =
  "owner_id";` to unlock CrudBase's `find_owned/list_owned/count_owned`, and
  gate the controller with `API_REQUIRE_OWNER`. Scaffold it with
  `new-resource.sh <Entity> --owned`. Plain `find`/`list` on an owned table is
  an IDOR.
- The repository must NOT know about HTTP. It throws; the controller maps.

## 4. Controller — `src/api/<Entity>Controller.hpp`

Model after `AdminController`. Includes: `api/Guards.hpp`,
`api/HandlerSupport.hpp`, `api/RequestUtils.hpp`, `api/Validation.hpp`,
`utils/ErrorResponse.hpp` — **never** `api/Api.hpp` (include cycle).

Per handler:
1. First line: a guard from `Guards.hpp` —
   `API_REQUIRE_ADMIN(req, callback)` or `API_REQUIRE_PRINCIPAL(req, callback, p)`.
   Every mutating endpoint needs one (under `AUTH_MODE=none` it's a no-op, so
   it never gets in the way during dev — but it's there for prod). Handlers of
   a toggleable feature module gate first with
   `if (!require_content_enabled(callback)) return;` (see `PostsController`;
   `new-module.sh` scaffolds the flag).
2. UUID path params: `if (!require_valid_uuid(id, callback)) return;`
   (`Guards.hpp`) — don't hand-roll the `is_valid_uuid` + 400 pair.
3. Parse + validate: `Validation::parse_body(req, body, callback)` then
   `Validation::require/email/string_length`, accumulate into
   `Validation::Errors`, bail with `Validation::response_400(errs)`.
4. Repository call inside `with_repo_errors(callback, "op", [&]{ … })`
   (`HandlerSupport.hpp`) — it maps `Duplicate*`→409, `*NotFound`→404,
   anything else→500-with-log. Don't hand-roll the catch ladder.
5. List endpoints: `parse_page_params(req, default, max)` →
   `Response::paginated(to_json_array(items), total, page.limit, page.offset)`
   (see `AdminController::listUsers`).
6. Success: `Response::ok(...)` / `Response::created(...)` — never build the
   `HttpResponse` by hand.
7. **`callback(...)` exactly once on every path**, including early returns and
   the guard expansions.

## 5. Route registry + OpenAPI

- Add the route to `Api::get_endpoints()` in **`src/api/Endpoints.hpp`** (NOT
  `Api.hpp`) for every `ADD_METHOD_TO`.
- Add the `#include "api/<Entity>Controller.hpp"` to `src/api/Api.hpp`.
- Add the path block + `components/schemas/<Entity>` to `docs/openapi.yaml`
  (hand-edited). Run `./scripts/check-openapi-drift.sh` — it verifies
  `(method, path)` parity (it does **not** check schema bodies, so review
  those by eye).

## 6. Frontend — `frontend/`

- Types come from `docs/openapi.yaml` via `npm run gen:api`
  (openapi-typescript → `src/lib/api/schema.gen.ts`); import the flat aliases
  from `@/lib/api/types`.
- Hook: `src/hooks/useAdmin<Entity>.ts` (model `useAdminRoles`); query keys
  from `src/lib/api/queryKeys.ts` (never inline string tuples).
- Page: `src/pages/admin/<Entity>.tsx` — `usePagedQuery` + `PaginationFooter`
  + `<DataTable>` for the list, `useApiMutation` for create/update/delete.
- Route: add under `<RequireAdmin>` in `src/App.tsx`.

## 7. Tests

- Integration suite `tests/integration/test_<entity>.cpp` extending
  `TestHelpers::CoreBackedTest` (`config_overrides`, `requires_postgres`,
  `post_init`); use `TestHelpers::make_request/authed/truncate_users`.
- Buckets are classified by DIRECTORY, not a filter list: a file in
  `tests/integration/` (or `tests/api/`) is compiled into the integration
  binary by CMake's `CONFIGURE_DEPENDS` glob with no registration step.
  `./scripts/check-test-buckets.sh` (run in CI) just guards placement — it
  fails on a suite-name clash across buckets, so a DB-dependent suite can't
  silently shadow a unit suite.

## What NOT to reach for

These were considered and rejected as overengineering for a starter template:
generic `Repository<T>` (the `execute_*` lambdas already are the base layer),
a service layer everywhere (`AccountEmails` is the one place reuse justified
it), an `IDatabase` interface for mocking (repositories are tested
integration-style against real Postgres), and a schema-driven form generator.
Keep handlers readable and the flow visible in one file.

## Gotchas — hard-won rules

These don't fall out of reading the code; they were learned the hard way.

1. **Header-only for leaves/templates; heavy module bodies in `app_core` `.cpp` (ADR 0003, amended 2026-08-22).** Leaf utilities, domain structs and ALL template code stay in `.hpp`. Modules past the `scripts/bench-incremental.sh` threshold get their non-template bodies de-inlined into a paired `src/<dir>/Foo.cpp` compiled ONCE into the `app_core` STATIC library — CMake GLOBs `src/**/*.cpp` (everything except `main.cpp` / `worker_main.cpp`, which stay per-binary), so a new module `.cpp` links with no CMake edit. De-inlined so far: billing (`Wallet`, `PayPalClient`, `BillingController`, `AdminBillingController`, `BillingEmails`), `core/Core`, `api/Middleware`, `api/Api`, and the utils hubs (`Config`, `Strings`, `ErrorResponse`, `Crypto` — template accessors like `Config::get<T>` stay in the header); Drogon route macros (`ADD_METHOD_TO` / `METHOD_LIST_*`) always stay in the controller `.hpp` — the route gates grep only headers.
2. **`Api::get_endpoints()` (`src/api/Endpoints.hpp`) is the single source of truth for routes.** After an `ADD_METHOD_TO(...)`, add the matching line there, or `scripts/check-openapi-drift.sh` fails CI and `--print-routes` won't show it. Controllers include `api/Guards.hpp` + `api/RequestUtils.hpp` but NOT `api/Api.hpp` (cycle).
3. **JSON: nlohmann::json only, never jsoncpp.** Drogon uses jsoncpp internally, but project code is all nlohmann. Parse with `json::parse(req->body())`, respond with `data.dump()` + `CT_APPLICATION_JSON`. **Never call `req->getJsonObject()`.**
4. **`AUTH_MODE=none` by default** — every endpoint public. On any mutating endpoint add a guard from `api/Guards.hpp` (`API_REQUIRE_ADMIN` / `API_REQUIRE_PRINCIPAL`) or `Security::Auth::require_role(...)`, or document why it's public.
5. **`Core::initialize()` has a strict init order:** Config → Observability → Database → Migrations → Cache → Storage → Messaging → Tasks → Security → Jobs → Mailer; shutdown is the reverse. Don't reorder (Cache uses Observability metrics; Database depends on Config).
6. **`req->attributes()->get<T>(key)` on a miss returns a default-constructed `T`** (older Drogon threw `std::out_of_range` — both exist). Never rely on the throw: `find(key)` first, then `get<T>` — see `Security::Auth::principal_of`. An empty principal reaching SQL as a uuid has crashed a handler before.
7. **Every Redis call is fail-open** — except counters. `CacheManager` methods swallow `sw::redis::Error` (via the private `guarded_` wrapper) and log a warn; but `incr`/`decr` log and RETHROW (a counter silently stuck at a default would corrupt rate accounting). Wrap direct `get_client()` calls in try/catch. See `docs/adr/`.
8. **Drogon's log level is hardcoded in `main.cpp` (`Logger::kInfo`).** `LOG_LEVEL` only affects spdlog. For Drogon debug, edit `setLogLevel` in `main.cpp`.
9. **`callback(...)` exactly once on every path** — including exceptions and early returns — or the client hangs until timeout.
10. **Tests don't call controller methods directly with a fake `req`** — use `tests/test_helpers.hpp` (`make_request(...)`).
11. **`docs/openapi.yaml` is edited by hand.** The drift checker compares `(method, path)`; update the spec on any route change.
12. **`make migrate-reset` / `make down-v` / `make dist-clean` are destructive** (drop data) — don't run them casually.
13. **Validate Helm before pushing chart changes: `make helm-validate`.** It renders the umbrella with `values-ci.yaml` and asserts deploy invariants (ingress port == service port, `baseDomain` expanded, `MAIL_VIA_JOBS` injected). Gotcha: `MAIL_VIA_JOBS` only renders on the api when `mi-fitness-api.mail.enabled: true` (`deployment.yaml` gates `{{- if .Values.mail.enabled }}`). Vendored `helm/*/charts/*.tgz` are gitignored — run `helm dependency build` (which `helm-validate` does) before rendering/deploying.
14. **PCH `REUSE_FROM` is a single fragile build point.** integration/e2e/worker tests `REUSE_FROM` the unit PCH; flag drift (esp. sanitizer defines) silently breaks reuse or forces a full rebuild — which is why the ASan build lives in a separate `build-san` tree. Changing the `HEAVY_PCH` set or flags → rebuild all test buckets AND the sanitizer build.
15. **Test fixtures never bind a Prometheus exposer.** `TestHelpers::minimal_config()` sets `observability.metrics_address` to `""` — registry only, no CivetWeb HTTP server. Binding one costs a FLAT ~2 s per test in TearDown (the exposer's listener thread parks in a 2000 ms poll quantum and `~Exposer` waits it out). The bind path is covered once, on a real port, in `tests/unit/test_observability.cpp`. Don't put a real `metrics_address` into a fixture config.
16. **Never edit an applied migration.** `schema_migrations` records a sha256 checksum per applied file; the runner refuses to boot when an applied file's on-disk hash differs. A database that already recorded version N will never re-run `NNN_*.sql`, so an in-place edit silently never reaches it — ship a new migration with idempotent re-declarations (`IF NOT EXISTS`, `DO $$` guards) instead.
17. **Multi-table test cleanup goes through `TestHelpers::wipe_app_data()`.** It is the one place that knows the FK order (users CASCADE before extra roles; posts/audit_log/used_tokens ride one TRUNCATE) and re-seeds migration 001's two roles. Don't hand-roll TRUNCATE/DELETE sequences in fixtures.
18. **Never accumulate with a self-referencing upsert** (`INSERT ... ON CONFLICT (k) DO UPDATE SET x = t.x + EXCLUDED.x`) through `Database::execute_write`. In a downstream fork every second-or-later write computed as though the existing row's value were 0 — invisible for positive deltas, fatal for negative ones (`CHECK` violations on refunds). CI evidence proved the conflicting row WAS detected and visible earlier in the same transaction, yet the self-referencing SET still read it as absent; the root cause was never found (forensics: site fork, commit b676430). Safe idiom: `INSERT ... ON CONFLICT DO NOTHING` (materialize the row so it can be locked) → `SELECT ... FOR UPDATE` → compute the new total in C++ → plain `UPDATE`.
19. **After-response side effects go through the 4-arg `with_repo_errors` overload** (email dispatch, enqueue, webhook fire): `after_fn` runs once the guarded block completed, OUTSIDE the catch ladder, so a throwing side effect can never fire `callback` a second time. Stash results into a `std::optional` captured by reference if `after_fn` needs them (learned downstream: a receipt-email dispatch inside the guarded lambda double-fired the callback).
20. **Events that must not be lost go through the transactional outbox** (`src/jobs/Outbox.hpp`, migration 010, opt-in via `OUTBOX_DRAIN_INTERVAL_SEC`). The after-response hook above is best-effort BY DESIGN: a process dying between the DB commit and `Jobs::submit` loses the event, and a failed submit is only logged. Fine for a re-requestable confirm-email; not fine for money paths, where "wallet debited but the receipt/webhook never fired" is a truth defect someone repairs by hand. There, write the event in the SAME transaction as the ledger write — commit makes both durable, rollback erases both — and the periodic drain relays it to the job queue (at-least-once; handlers must tolerate a duplicate, which Jobs' retry/lease paths already require). A billing-fork receipt dispatch is three lines inside the existing wallet transaction:
    ```cpp
    // inside Database::execute_transaction([&](auto& txn) { ... ledger write ... 
    if (result.credited)  // same dedupe gate BillingEmails::receipt documents
        Jobs::Outbox::enqueue(txn, Email::SendEmail::kJobType,
                              {{"to", user.email}, {"subject", subject}, {"text", text}});
    ```
    The template's own email flows deliberately stay on the after-response path (bit-for-bit unchanged behaviour) — the outbox is the upgrade a fork opts specific call sites into, not a global rewire.

### Frontend gotchas

- **`schema.gen.ts`** is a committed stub so `git clone` builds; `npm run gen:api` (or `make frontend-gen-api`) regenerates it from `docs/openapi.yaml`. CI always regenerates before `tsc`.
- **HttpOnly cookies — not readable from JS.** Sessions live in `__Host-access` + `__Host-refresh`; the SPA relies on `useMe()` (TanStack Query) to check auth.
- **Same-origin only.** Dev: Vite proxy `/api → :8080`. Prod: nginx `proxy_pass /api → app:8080`. No CORS config needed.
- **Permission bitmask mirror.** `frontend/src/lib/auth/permissions.ts` duplicates `Domain::Permission::k*` — change both together (a drift test enforces it).
- **No auto-login after register** — the flow goes to `/account/check-email`; the user clicks the email link, then logs in.
- **Self-protection in the admin UI.** The backend returns 400 on self-delete / self-role-change; the UI disables those controls when `me.id === target.id`.

### Where to look (question → file)

The scaffolding scripts are the entry points:

| Want to… | Look at / run |
|---|---|
| Add a full CRUD resource | `./scripts/new-resource.sh` |
| Add a backend endpoint | `./scripts/new-endpoint.sh` |
| Add a feature-module flag | `./scripts/new-module.sh` |
| Add a migration | `./scripts/new-migration.sh` |
| Add a frontend page | `./scripts/new-react-page.sh` |
| Bring up the whole stack | `make up && make frontend-up` |
| Create an admin | `<bin> --create-admin EMAIL [PASS]` |
| Debug routes / health / traces | `make routes` / `make health` / `make tail-trace TID=<id>` |
| Regenerate frontend types | `make frontend-gen-api` |
| Run "like CI" locally | `make ci-local` |
| Check spec ↔ code drift | `./scripts/check-openapi-drift.sh` |
| Validate Helm render | `make helm-validate` |
| Worked CRUD example | `docs/EXAMPLES.md` |
| Must-not-lose event dispatch (outbox vs after-response) | `src/jobs/Outbox.hpp` (doc comment is the decision guide; gotcha 20 above has the worked example) |
| Money / ledger / payment-provider reference | `src/billing/Wallet.hpp` (billing module: append-only ledger, idempotent capture/refund/spend — spend is reference-keyed via migration 009's partial unique index, no HTTP endpoint (charging is fork domain), integer-only money; best-effort emails in `src/email/BillingEmails.hpp`) |
| ADRs / architecture decisions | `docs/adr/` |
