# End-to-end CRUD example

The template is **not** bare. It already ships two working domains you can
read as the canonical pattern:

- auth / RBAC / admin: `migrations/001_users_and_roles.sql` (`users` + `roles`
  + permission bitmask), `src/domain/{User,Role}.hpp`,
  `src/repositories/{User,Role}Repository.hpp`,
  `src/api/{Auth,Account,Admin}Controller.hpp`
- the content module (off by default, `CONTENT_ENABLED=false`):
  `migrations/006_add_posts.sql`, `src/domain/Post.hpp`,
  `src/repositories/PostRepository.hpp`, `src/api/PostsController.hpp`

All routes are registered in `src/api/Endpoints.hpp`.

**`src/repositories/PostRepository.hpp` + `src/api/PostsController.hpp` are
the canonical, real-world CRUD example** — a `new-resource.sh` scaffold grown
into production code (CrudBase constants, `translate_sql`/`throw_on`, guards,
`with_repo_errors`). `UserRepository` is the hand-written variant (it joins
roles). Read those first. This doc is a *second*, self-contained walkthrough
for adding **your own** new resource on top of what already ships, without
touching the shipped tables.

To avoid colliding with the shipped schema (migrations `000`–`006` are taken),
the walkthrough below introduces a fresh `notes` resource and starts at
migration `007`. Adapt the name to your project.

Target directory layout (adding a new `notes` resource alongside the shipped code):

```
migrations/
  001_users_and_roles.sql      # already shipped — do NOT rewrite
  006_add_posts.sql            # already shipped (content module)
  007_notes.sql                # your new resource
src/
  api/
    NotesController.hpp        # HTTP controller
  domain/
    Note.hpp                   # Typed DTO + nlohmann::to_json
  repositories/
    NoteRepository.hpp         # All SQL lives here
tests/
  integration/
    test_notes.cpp             # Exercises the stack end-to-end
```

The `Domain` / `Repositories` namespaces below match the shipped code — the
new types (`Domain::Note`, `Repositories::NoteRepository`) just join the same
namespaces. Rename `Note` to whatever your resource is.

---

## 1. Migration

`migrations/007_notes.sql` (run `scripts/new-migration.sh notes` to get the
next-numbered skeleton — `000`–`006` are already taken by the shipped schema):

```sql
-- pgcrypto (gen_random_uuid) is already enabled by migration 001, but
-- CREATE EXTENSION IF NOT EXISTS is idempotent, so re-declaring is harmless.
CREATE EXTENSION IF NOT EXISTS "pgcrypto";

CREATE TABLE IF NOT EXISTS notes (
    id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    title      VARCHAR(255) NOT NULL,
    body       TEXT NOT NULL DEFAULT '',
    -- Tie a note back to its author. ON DELETE CASCADE so removing a user
    -- removes their notes (adjust to your needs).
    author_id  UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    published  BOOLEAN NOT NULL DEFAULT FALSE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_notes_author_id ON notes (author_id);

-- updated_at maintenance: attach the SHARED touch_updated_at() function from
-- migration 000 — don't duplicate the function per table (001 keeps its own
-- users_touch_updated_at() only for flask-base parity).
DROP TRIGGER IF EXISTS notes_touch_updated_at ON notes;
CREATE TRIGGER notes_touch_updated_at BEFORE UPDATE ON notes
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();
```

`MigrationRunner` applies `007` after `006` on boot, in numeric order (or run
`--verify-migrations` to check drift). Do NOT add `BEGIN`/`COMMIT` — the runner
wraps each file in one transaction under an advisory lock (see the note at the
top of `000_updated_at_trigger.sql`).

---

## 2. Typed DTO

`src/domain/Note.hpp` (the shipped `src/domain/User.hpp` / `Post.hpp` are the
real versions of this pattern — open one side by side):

```cpp
#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

namespace Domain {

struct Note {
    std::string id;
    std::string title;
    std::string body;
    std::string author_id;
    bool published{false};
    std::string created_at;
    std::string updated_at;

    template <typename Row>
    static Note from_row(const Row& row) {
        Note n;
        n.id = row["id"].template as<std::string>();
        n.title = row["title"].template as<std::string>();
        n.body = row["body"].template as<std::string>();
        n.author_id = row["author_id"].template as<std::string>();
        n.published = row["published"].template as<bool>();
        n.created_at = row["created_at"].template as<std::string>();
        n.updated_at = row["updated_at"].template as<std::string>();
        return n;
    }
};

inline void to_json(nlohmann::json& j, const Note& n) {
    j = nlohmann::json{
        {"id", n.id}, {"title", n.title}, {"body", n.body},
        {"author_id", n.author_id}, {"published", n.published},
        {"created_at", n.created_at}, {"updated_at", n.updated_at},
    };
}

}  // namespace Domain
```

Notes:
- Timestamps stay as strings — the DB hands us ISO8601 and the API echoes
  the same shape, so no chrono round-trip is needed.
- Nullable columns become `std::optional<std::string>` and the `to_json`
  branch picks `nullptr` for empty optionals so the wire format stays stable.

---

## 3. Repository

`src/repositories/NoteRepository.hpp`. This is the shape
`./scripts/new-resource.sh Note` scaffolds (the shipped `PostRepository` is
the grown-up version of it): extend `CrudBase`, declare four constants, and
hand-write only the bespoke writes. `CrudBase`
(`src/repositories/CrudBase.hpp`) supplies `find(id)` / `list(limit, offset)` /
`count()` from those constants, so you don't re-implement the mechanical reads.
The shipped `RoleRepository` / `PostRepository` are the production versions of
this exact pattern; `UserRepository` is a hand-written variant (it joins roles,
so it keeps its own queries).

```cpp
#pragma once

#include <optional>
#include <stdexcept>
#include <string>

#include "database/Database.hpp"
#include "domain/Note.hpp"
#include "repositories/CrudBase.hpp"
#include "repositories/RepoErrors.hpp"  // NotFoundError / ConflictError bases
#include "repositories/SqlErrors.hpp"   // detail::translate_sql + detail::throw_on

namespace Repositories {

// Derive from the generic bases so Api::with_repo_errors maps them to the right
// status (404 / 409) WITHOUT the shared handler knowing this concrete type.
struct NoteNotFound : NotFoundError {
    NoteNotFound() : NotFoundError("note") {}
};

class NoteRepository : public CrudBase<NoteRepository, Domain::Note, std::string> {
public:
    // CrudBase supplies find(id) / list(limit, offset) / count() from these four.
    static constexpr const char* kTable    = "notes";
    static constexpr const char* kColumns  = "id, title, body, author_id, published, created_at, updated_at";
    static constexpr const char* kIdColumn = "id";
    static constexpr const char* kOrderBy  = "created_at DESC";

    // Only the writes are bespoke. Wrap UNIQUE/FK-tripping writes in
    // detail::translate_sql so a SQLSTATE becomes a typed exception, not a 500 —
    // detail::throw_on<DuplicateNote>("23505") is the ready-made translator for
    // the single-SQLSTATE case (see PostRepository::create for the real thing).
    Domain::Note create(const std::string& title, const std::string& body, const std::string& author_id) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                std::string("INSERT INTO notes (title, body, author_id) VALUES ($1, $2, $3) RETURNING ") + kColumns,
                title, body, author_id);
            return Domain::Note::from_row(r[0]);
        });
    }

    // Hard delete — the template has NO soft-delete (no deleted_at / is_active).
    // For soft-delete: add a `deleted_at TIMESTAMPTZ` column and filter
    // `WHERE deleted_at IS NULL` in every read.
    void remove(const std::string& id) {
        Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params("DELETE FROM notes WHERE id = $1 RETURNING id", id);
            if (r.empty()) throw NoteNotFound{};
            return 0;
        });
    }
};

}  // namespace Repositories
```

**Per-user (owner-scoped) resources.** If a `Note` belongs to a user, generate
it with `./scripts/new-resource.sh Note --owned`. The migration gets an
`owner_id` FK, the repo declares `static constexpr const char* kOwnerColumn =
"owner_id";` (which unlocks CrudBase's `find_owned(id, owner)` /
`list_owned(owner, …)` / `count_owned(owner)`), and the controller gates with
`API_REQUIRE_OWNER` and passes the caller as the owner — so one user can **never**
read or delete another's rows. Reaching for the plain `find`/`list` on a
user-owned table is an IDOR; the owner-scoped methods exist so you don't.

Key rules the repository enforces, not the controller:

- Every SQL string for the `notes` table lives here — controllers never touch
  pqxx. The lambda takes `[&](auto& txn)`: `execute_read` / `execute_write` hand
  it a `detail::TracingTxn&`, not a raw `pqxx::work&` / `pqxx::read_transaction&`.
- Constraint violations surface as typed exceptions deriving from
  `NotFoundError` / `ConflictError` (`repositories/RepoErrors.hpp`); translate
  SQLSTATE with `detail::translate_sql` + `detail::throw_on`
  (`repositories/SqlErrors.hpp`), exactly like `UserRepository::create` maps
  `23505` → `DuplicateEmail`. The HTTP layer then maps them to 404 / 409
  without string-sniffing or knowing the type.
- `find` returns `std::optional` — the controller decides 404 vs cache miss.
- Use `execute_read_primary` (not `execute_read`) right after a write that the
  same request re-reads — replica lag can otherwise return a stale / not-found row.

---

## 4. Thin controller

`src/api/NotesController.hpp` (abbreviated — list + get + create only). The
shipped `PostsController.hpp` / `AdminController.hpp` are the real-world
versions of a guarded CRUD controller:

```cpp
#pragma once

#include <drogon/HttpController.h>
#include <drogon/drogon.h>

// Controllers do NOT include "api/Api.hpp" — that header includes every
// controller, so pulling it back in is a circular include.
#include "api/Guards.hpp"          // API_REQUIRE_* + require_valid_uuid
#include "api/HandlerSupport.hpp"  // with_repo_errors, to_json_array
#include "api/RequestUtils.hpp"    // parse_int, parse_page_params, ...
#include "api/Validation.hpp"
#include "cache/Cache.hpp"
#include "domain/Note.hpp"
#include "repositories/NoteRepository.hpp"
#include "security/Auth.hpp"        // Security::Auth::principal_of
#include "utils/ErrorResponse.hpp"  // Response::ok / created / paginated, ErrorResponse::*

namespace Api {

using namespace drogon;

class NotesController : public HttpController<NotesController> {
public:
    METHOD_LIST_BEGIN
    // Business routes live under /api/v1 (ADR 0006) — new-endpoint.sh rejects
    // unversioned paths.
    ADD_METHOD_TO(NotesController::listNotes, "/api/v1/notes", Get);
    ADD_METHOD_TO(NotesController::createNote, "/api/v1/notes", Post);
    ADD_METHOD_TO(NotesController::getNoteById, "/api/v1/notes/{1}", Get);
    METHOD_LIST_END

    void listNotes(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback) {
        const auto page = parse_page_params(req, /*default_limit=*/50, /*max_limit=*/200);
        with_repo_errors(callback, "listNotes", [&] {
            Repositories::NoteRepository repo;
            auto items = repo.list(page.limit, page.offset);
            long total = repo.count();
            // The standard paginated envelope: {data, total, limit, offset}.
            callback(Response::paginated(to_json_array(items), total, page.limit, page.offset));
        });
    }

    void getNoteById(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback,
                     const std::string& id) {
        if (!require_valid_uuid(id, callback))
            return;

        // Cache-aside, fail-open: a Redis hiccup must never block the read
        // (the CONVENTIONS gotchas). CacheManager already swallows redis errors
        // internally; the is_initialized() guard covers the not-booted case.
        const std::string cache_key = "note:" + id;
        if (Cache::is_initialized()) {
            if (auto cached = Cache::get().get(cache_key)) {
                callback(Response::ok({{"data", json::parse(*cached)}, {"source", "cache"}}));
                return;
            }
        }

        with_repo_errors(callback, "getNoteById", [&] {
            auto found = Repositories::NoteRepository{}.find(id);
            if (!found) {
                callback(ErrorResponse::not_found("note"));
                return;
            }
            json data(*found);
            if (Cache::is_initialized())
                Cache::get().set(cache_key, data.dump(), /*ttl=*/300);
            callback(Response::ok({{"data", data}, {"source", "database"}}));
        });
    }

    void createNote(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback) {
        // Mutating endpoint: gate it. API_REQUIRE_PRINCIPAL resolves the
        // authenticated principal into `me` (a std::optional<AuthPrincipal>) or
        // rejects with 401 and returns. Drop this only if the route is
        // intentionally public (and say why). Use API_REQUIRE_ADMIN instead
        // for admin-only routes.
        API_REQUIRE_PRINCIPAL(req, callback, me);
        json body;
        if (!Validation::parse_body(req, body, callback)) return;

        Validation::Errors errs;
        Validation::require(errs, body, "title");
        Validation::string_length(errs, body, "title", 1, 255);
        if (errs.any()) { callback(Validation::response_400(errs)); return; }

        // The author is the logged-in principal, not a client-supplied field.
        // AuthPrincipal::subject holds the user id (see src/security/Auth.hpp).
        const std::string author_id = me->subject;

        with_repo_errors(callback, "createNote", [&] {
            auto n = Repositories::NoteRepository{}.create(
                body["title"].get<std::string>(),
                body.value("body", std::string{}),
                author_id);
            callback(Response::created({{"data", json(n)}, {"message", "Note created"}}));
        });
    }
};

}  // namespace Api
```

Controller responsibilities — and nothing else:

1. Parse + validate the request.
2. Delegate to the repository inside `with_repo_errors` (it maps the typed
   exceptions to 404 / 409 / 500 — don't hand-roll the catch ladder).
3. Cache-aside where it helps (inline via `Cache::get()`, fail-open).
4. Serialize the DTO back out via `to_json`.

`Response::ok` / `Response::created` / `Response::paginated` live in
`utils/ErrorResponse.hpp` (namespace `Response`); `require_valid_uuid` is in
`src/api/Guards.hpp`; `to_json_array` and `with_repo_errors` are in
`src/api/HandlerSupport.hpp`; `parse_page_params` is an inline helper in
`src/api/RequestUtils.hpp` (alongside `parse_int`, `clamp_int`,
`normalize_path_for_metrics`). The HTTP middleware
wired in `Api::register_controllers()` (bodies in `src/api/Middleware.cpp`)
handles tracing spans — handlers don't open their own.

If your resource is a toggleable feature module (like the shipped content
module), every handler opens with `if (!require_content_enabled(callback))
return;` — see `PostsController` and `./scripts/new-module.sh` for the
config-flag wiring.

Two wiring steps the drift checker holds you to:

1. `#include` the controller from `src/api/Api.hpp` so Drogon picks it up.
2. Add a row to `Api::get_endpoints()` in **`src/api/Endpoints.hpp`** (the
   route registry, moved out of `Api.hpp`) and a matching path block in
   `docs/openapi.yaml` — one row per `ADD_METHOD_TO`. Skip either and
   `scripts/check-openapi-drift.sh` / `scripts/check-routes-registered.sh`
   turn CI red.

---

## 5. Integration test

`tests/integration/test_notes.cpp` (sketch):

```cpp
#include <gtest/gtest.h>
#include "api/NotesController.hpp"
#include "test_helpers.hpp"

class NotesIntegration : public TestHelpers::CoreBackedTest {
protected:
    bool requires_postgres() const override { return true; }
};

TEST_F(NotesIntegration, CreateAndFetch) {
    // CoreBackedTest boots Core once (override config_overrides() /
    // post_init() as needed) and skips the suite when requires_postgres()
    // can't be satisfied. POST /api/v1/notes via TestHelpers::make_request,
    // GET it back, assert equality.
}
```

The template ships `TestHelpers::CoreBackedTest` (with `config_overrides`,
`requires_postgres`, `post_init` hooks), `TestHelpers::make_request(method[, body])`,
`authed(principal[, method])`, `truncate_users()`, `wipe_app_data()`, and
`drain_jobs({types})` — lean on those instead of hand-rolling a fixture.

Note: `truncate_users()` wipes the **shipped auth `users` table** — it is not a
generic helper (multi-table cleanup of the shipped schema goes through
`wipe_app_data()`, which knows the FK order). For your `notes` resource you
need a matching truncate of your own table, e.g.:

```cpp
inline void truncate_notes() {
    Database::get().execute_write([](auto& txn) {
        txn.exec("TRUNCATE TABLE notes");
        return 0;
    });
}
```

(`notes.author_id` references `users(id)` with `ON DELETE CASCADE`, so wiping
`users` also clears dependent `notes` — but a dedicated `truncate_notes()`
keeps the intent explicit and works even if you drop the FK.)

---

## Why keep this as a doc instead of shipping it as demo code?

- The shipped surface stays focused: auth / RBAC / admin plus the (optional,
  off-by-default) content module are the real domains in `src/`, so grep
  surveys and static analysis don't return throwaway demo noise on top of them.
- `docs/openapi.yaml` tracks only endpoints that actually exist, so spec-drift
  checks don't fight sample data that was never wired up.
- The `Note` walkthrough lives here, fully copy-pasteable, so you add it (at
  the next free migration number) when you need it instead of deleting it when
  you don't.

What **is** shipped and ready to read as the canonical pattern:
`migrations/000`–`006`, `src/domain/{User,Role,Post,ApiKey,AuditEntry}.hpp`,
`src/repositories/{User,Role,Post,ApiKey,Audit}Repository.hpp` (+ `CrudBase`),
and `src/api/{Auth,Account,Admin,Posts,ApiKey,Audit}Controller.hpp` (routes in
`src/api/Endpoints.hpp`).
