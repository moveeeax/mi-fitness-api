# Multi-tenancy (organizations) starter kit

Installed by `./scripts/add-orgs.sh` — a ONE-SHOT generator, not a runtime
module. Multi-tenancy is an architectural decision: every org-scoped table
carries `org_id NOT NULL`, every read is scoped by it, and a "half-disabled"
mode would be an illusion of isolation. If your fork needs tenants, run the
script once and own the generated code; if it doesn't, don't install it.

A fresh fork that already knows it needs tenants can install the kit as part
of bootstrap: `./scripts/init-project.sh --with-orgs …` runs `add-orgs.sh`
right after the rename (the patch anchors are name-independent, so before /
after the rename makes no difference; it composes with `--minimal` too).

What lands: `src/tenancy/*` (domain, repositories, `OrgContext`,
`OrgCrudBase`, permission matrix), `src/api/OrganizationsController.hpp`
(org CRUD, member management, `/switch`), org guards in `src/api/Guards.hpp`,
the `organizations`/`org_members` migration, org-claim minting in
`AuthController`, routes in `Endpoints.hpp` + `openapi.yaml`, and four test
suites. Then scaffold tenant resources with
`./scripts/new-resource.sh <Entity> --org-scoped`.

## Two role layers — don't conflate them

| Layer | Lives in | Answers | Checked by |
|---|---|---|---|
| System permission bits | `roles.permissions` → JWT `permissions` claim | "may this user administer the PLATFORM?" | `API_REQUIRE_ADMIN` / `API_REQUIRE_PERMISSION` |
| Tenant role (`owner`/`member`/`viewer`) | `org_members.role` | "what may this user do INSIDE this org?" | `API_REQUIRE_ORG` + `API_REQUIRE_ORG_PERM` |

A platform admin is not automatically inside any org (they can fall back to
the admin path on member-management routes, and they become `owner` of orgs
they create). A tenant `owner` is not a platform admin.

Tenant grants live in one place: the matrix in
`src/tenancy/OrgPermissions.hpp`, positional over the role list,
**deny-by-default in all three dimensions** — unknown role, unknown resource
and unknown action are all 403. A resource you forget to add a row for is
closed, not open. Empty grant `""` means invisible (gate reads too), and the
`static_assert`s pin role-column order so a reorder can't silently shift
every grant. Unit-tested without infra in
`tests/unit/test_org_permissions.cpp` — keep `kAllResources` there in sync
with `kMatrix` (a `static_assert` enforces it).

## The `org` claim: minted only when unambiguous

`mint_session` (login **and** refresh) sets the access token's `org` claim
only when the user belongs to **exactly one** organization. Zero memberships:
nothing to default to. More than one: the client must choose explicitly via
`POST /api/v1/orgs/{id}/switch`, which mints a new access token with
`org={id}` (and rewrites the access cookie in cookie mode; the refresh
token/cookie is never touched). The claim is recomputed from live membership
on every mint, so a stale value self-heals instead of being carried forward.

**Documented UX trap (hit in the origin fork): a switch does not survive a
refresh.** `POST /auth/refresh` re-runs the single-membership rule, so a
multi-org user's active org resets to "none" on every token rotation. The SPA
must re-issue `/switch` after each refresh (or persist the chosen org id and
re-switch on 403). `GET /auth/me` returns `org_role` (the tenant role behind
the current claim, `null` when unscoped) so the client can detect the reset.

## Fail-closed context + instant revocation

`Tenancy::org_context_of()` yields a context only when the principal carries
a non-empty `org` claim **and** a live `org_members` row backs it. That
per-request membership read is deliberate: removing someone from an org
revokes their access on their very next request — not at access-token expiry.
Don't cache it without accepting the revocation delay a TTL implies.

`API_REQUIRE_ORG` is **not** a no-op under `AUTH_MODE=none` (unlike the admin
guards): tenant data is meaningless without an identity plus a membership, so
the orgs kit requires real auth. Keep `auth.mode=jwt`.

## Structural isolation for reads

Org-scoped repositories inherit `Tenancy::OrgCrudBase`, which offers
`find_in_org` / `list_in_org` / `count_in_org` and **no** global
`find`/`list`/`count` at all — a forgotten org filter is a compile error, not
a runtime IDOR. A cross-org id lookup returns `nullopt`, indistinguishable
from a missing row (no existence leak). `organizations` itself is the one
org-side table on the global `CrudBase` (tenants are platform-level rows).

## Known gap

Last-owner protection (409 `last_owner` on demoting/removing an org's only
owner) counts owners and mutates in separate transactions; two concurrent
demotions in a two-owner org can race past it. Close it with a single guarded
SQL statement before shipping member self-service at scale.
