# Removing the reference / demo material

This template ships some **pedagogical** content that exists to explain *why* the
code is shaped the way it is. A real fork doesn't need it. This file says exactly
what is reference-only (safe to delete) and what is the actual application (keep).

## TL;DR

```bash
# Strips the reference material as part of initialising your fork:
./scripts/init-project.sh --no-demo my-service docker.io/myorg example.org

# --no-demo PLUS removal of the content module (posts/uploads/sitemap):
./scripts/init-project.sh --minimal my-service docker.io/myorg example.org

# …or remove the reference material by hand at any time:
rm -rf _reference docs/PATTERNS-FROM-FLASK-BASE.md

# …and the content module alone, scripted (what --minimal runs internally):
./scripts/remove-content-module.sh
```

`--no-demo` also scrubs the now-dangling doc links and strips the README
"Live demo" block (demo URL + public demo credentials).

## The content module (`--minimal` / `remove-content-module.sh`)

The posts/uploads/sitemap feature set (PR #11) is the template's **worked
example** of a full feature module — real code, but demo-weight for a fork
that doesn't publish articles. `scripts/remove-content-module.sh` deletes it
whole while keeping every sync gate green by construction:

- **Deleted:** `PostsController` / `UploadController` / `ContentPagesController`
  (+ `.cpp` bodies), `Domain::Post`, `PostRepository`, `src/storage/`,
  `migrations/006_add_posts.sql`, its unit/integration/e2e tests, the admin
  SPA pages (`Posts.tsx`, `Media.tsx`).
- **Patched in lock-step with the gates:** `Endpoints.hpp` rows +
  `docs/openapi.yaml` blocks (route triple-sync), `config.json`/`sample` +
  `docs/CONFIG.md` + helm env/ConfigMap wiring (config-sync), both nginx
  configs (frontend-nginx-sync), `docs/module-deps.txt` (module DAG), the
  `public_paths` defaults in `Utils::Strings` and `config.json`.
- **Regenerated:** `frontend/src/lib/api/schema.gen.ts` from the shrunk spec
  (CI fails on a stale copy; needs `npm` — the script tells you if it
  couldn't).

Mechanics: shared files carry `init-project:content:start` / `…:end` marker
comments; the script strips those blocks, deletes the module's own files and
pattern-edits the JSON/YAML that can't carry markers. It is one-shot and
refuses to run twice.

Known cosmetic leftovers (deliberate): migration numbering keeps a gap at
006 (the runner sorts, contiguity is not required); narrative docs
(`docs/EXAMPLES.md`, `docs/CONVENTIONS.md`, ADRs, `docs/superpowers/`
archives) and a few comments citing `S3Storage` as a design precedent still
mention the module as history.

## What is reference-only (safe to delete)

| Path | What it is | Why it's removable |
| --- | --- | --- |
| `_reference/flask-base/` | A gitignored, depth-1 local clone (~21 MB) of the upstream Python **flask-base** source — never committed, so it exists only if you fetched it. | The C++ app mirrors its patterns (auth flows, permission bitmask, email tokens); the clone is here only so you can diff behaviour. Nothing builds or imports it. |
| `docs/PATTERNS-FROM-FLASK-BASE.md` | The mapping doc: "flask-base did X → here it's Y". | Pure narrative. No code references it at runtime. |

Deleting these does **not** touch any C++ target, migration, test, Helm chart, or
CI job — `init-project.sh` already excludes `_reference/` from its renaming pass,
and `--no-demo` only removes the two paths above plus the README "Live demo" block.

## What is NOT a demo — keep it

The application itself is production scaffolding, not a showcase. Keep all of it:

- **Auth**: register / login / logout / refresh, JWT + session cookies, password
  hashing, email-confirmation & reset tokens.
- **Domain**: `User` / `Role` / `AuditEntry` and their repositories — this is your
  real user system, not sample data. The admin API and audit trail are real.
- **Infra seams**: jobs/worker, cache, rate-limiting, idempotency, the resource
  scaffolder (`scripts/new-resource.sh`), Helm charts, and the CI pipeline.

If you want a *truly* minimal start, delete your own unused **feature** code
(controllers/repositories you scaffold and abandon) — but the auth/User/Role/Audit
core is the point of the template.

> Note: the "demo" Docker images referenced in the CHANGELOG and the `env-stage`
> deployment referenced in the `helm/cpp-env` values are *deployment* showcases
> (published images, a staging namespace),
> independent of the source tree. Removing the reference material above has no
> effect on them.
