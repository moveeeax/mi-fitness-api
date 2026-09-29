#!/usr/bin/env bash
#
# One-shot installer for the multi-tenancy (organizations) starter kit.
#
# Multi-tenancy is an ARCHITECTURAL decision, not a runtime flag: every
# org-scoped domain table carries org_id NOT NULL, every read is scoped by it,
# and a half-disabled mode would only be an illusion of isolation. So this is
# a GENERATOR you run once in a fork that needs tenants — not a module you
# toggle. Ported from the cyber-accountant fork (issue #25), generalized.
#
# Installs:
#   src/tenancy/Organization.hpp             tenant row (id/name/status)
#   src/tenancy/OrgMember.hpp                membership row (org, user, role)
#   src/tenancy/OrganizationRepository.hpp   SQL for `organizations`
#   src/tenancy/OrgMemberRepository.hpp      SQL for `org_members`
#   src/tenancy/OrgContext.hpp               fail-closed org resolver (claim + live membership)
#   src/tenancy/OrgScoped.hpp                OrgCrudBase — org-scoped repo base, NO global reads
#   src/tenancy/OrgPermissions.hpp           tenant-role permission matrix (deny-by-default)
#   src/api/OrganizationsController.hpp      org CRUD + member management + /switch
#   migrations/NNN_organizations.sql         organizations + org_members (next free NNN)
#   tests: unit/test_org_permissions, integration/test_org_context,
#          integration/test_org_scoped_crud, api/test_organizations_api
# Patches (anchor-checked, verified after insertion):
#   src/api/Guards.hpp          API_REQUIRE_ORG / API_REQUIRE_ORG_PERM macros
#   src/api/Api.hpp             #include of the controller
#   src/api/AuthController.cpp  org claim mint (login/refresh) + org_role in /auth/me
#   src/api/Endpoints.hpp       the 8 org routes (triple-sync)
#   docs/openapi.yaml           the 8 org routes (triple-sync)
#
# After this script the tree passes check-openapi-drift / check-routes-registered
# by construction. Org-scoped resources are then scaffolded with
# `./scripts/new-resource.sh <Entity> --org-scoped`.
#
# NOT idempotent by design: it refuses to run twice (the second run would have
# to merge into files you may have edited). See docs/ORGS.md for the concepts.
#
# Usage:
#   ./scripts/add-orgs.sh
set -euo pipefail

die() {
    echo "ERROR: $*" >&2
    exit 1
}

if [[ $# -gt 0 ]]; then
    echo "Usage: $0    (no arguments — one-shot installer; see docs/ORGS.md)" >&2
    exit 2
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TENANCY="$ROOT/src/tenancy"
GUARDS="$ROOT/src/api/Guards.hpp"
API_HPP="$ROOT/src/api/Api.hpp"
# The de-inlining pass (ADR 0003 as amended; PR #46) moved AuthController's
# bodies into the .cpp — the org-claim mint and /auth/me patch sites live
# there now, at method-body (4-space) indentation.
AUTH_CTRL="$ROOT/src/api/AuthController.cpp"
AUTH_HPP="$ROOT/src/security/Auth.hpp"
ENDPOINTS_HPP="$ROOT/src/api/Endpoints.hpp"
OPENAPI="$ROOT/docs/openapi.yaml"
ORGS_CTRL="$ROOT/src/api/OrganizationsController.hpp"

# ── Preconditions ────────────────────────────────────────────────────────
# Refuse to run twice: a re-run would clobber files the fork has since made
# domain-specific (the whole point of installing them as source, not a lib).
if [[ -e "$TENANCY/OrgScoped.hpp" || -e "$ORGS_CTRL" ]]; then
    die "the orgs starter kit is already installed (src/tenancy/ and/or \
src/api/OrganizationsController.hpp exist). add-orgs.sh is one-shot — to \
re-install, remove the generated files first (git can tell you which: they \
all arrived in the commit that ran this script)."
fi
grep -q "API_REQUIRE_ORG" "$GUARDS" && die "src/api/Guards.hpp already has org guards — partial install? Clean up first."
# The passive half this kit builds on: AuthPrincipal::org + the "org" claim
# parse ship in the template's Auth.hpp. An old fork that predates them must
# pull template master first (docs/UPSTREAM.md).
grep -q "std::string org;" "$AUTH_HPP" ||
    die "src/security/Auth.hpp has no AuthPrincipal::org field — your fork predates the orgs starter; sync with template master first (docs/UPSTREAM.md)"

for f in "$GUARDS" "$API_HPP" "$AUTH_CTRL" "$ENDPOINTS_HPP" "$OPENAPI"; do
    [[ -f "$f" ]] || die "$f not found — run from a template checkout"
done

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Insert the contents of fragment-file $3 immediately BEFORE the first line
# exactly equal to $2 in file $1.
insert_before() {
    local file="$1" anchor="$2" fragment="$3"
    grep -qxF "$anchor" "$file" || die "anchor '$anchor' not found in $file — patch by hand"
    awk -v anchor="$anchor" -v fragment="$fragment" '
        $0 == anchor && !done {
            while ((getline l < fragment) > 0) print l
            close(fragment)
            done = 1
        }
        { print }
    ' "$file" >"$file.tmp" && mv "$file.tmp" "$file"
}

# Same, but AFTER the anchor line.
insert_after() {
    local file="$1" anchor="$2" fragment="$3"
    grep -qxF "$anchor" "$file" || die "anchor '$anchor' not found in $file — patch by hand"
    awk -v anchor="$anchor" -v fragment="$fragment" '
        { print }
        $0 == anchor && !done {
            while ((getline l < fragment) > 0) print l
            close(fragment)
            done = 1
        }
    ' "$file" >"$file.tmp" && mv "$file.tmp" "$file"
}

mkdir -p "$TENANCY"

# ── 1. src/tenancy/Organization.hpp ──────────────────────────────────────
cat >"$TENANCY/Organization.hpp" <<'EOF'
/**
 * @file Organization.hpp
 * @brief Organization row (tenant). Mirrors the `organizations` table.
 *        Installed by scripts/add-orgs.sh.
 *
 * Domain-only — no SQL here; persistence lives in
 * src/tenancy/OrganizationRepository.hpp. Follows the same from_row/to_json
 * idioms as src/domain/User.hpp: from_row is a templated static factory
 * (works with any pqxx row-like type), and to_json is a free function found
 * via ADL so `nlohmann::json j = org;` "just works".
 *
 * Deliberately minimal: id/name/status/timestamps. Your fork's tenant almost
 * certainly has more identity than a name (tax id, billing plan, locale…) —
 * add those columns in a follow-up migration and mirror them here, keeping
 * struct / from_row / to_json in sync.
 */

#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace Tenancy {

struct Organization {
    std::string id;
    std::string name;
    std::string status;  // 'active' | 'suspended' | 'archived'
    std::string created_at;
    std::string updated_at;

    template <typename Row>
    static Organization from_row(const Row& row) {
        Organization o;
        o.id = row["id"].template as<std::string>();
        o.name = row["name"].template as<std::string>();
        o.status = row["status"].template as<std::string>();
        o.created_at = row["created_at"].template as<std::string>();
        o.updated_at = row["updated_at"].template as<std::string>();
        return o;
    }
};

/// Public JSON shape — everything on the row is safe to expose (no secrets).
inline void to_json(nlohmann::json& j, const Organization& o) {
    j = nlohmann::json{
        {"id", o.id},
        {"name", o.name},
        {"status", o.status},
        {"created_at", o.created_at},
        {"updated_at", o.updated_at},
    };
}

/// The tenancy roles org_members.role is CHECK-constrained to (see the
/// migration this script generated). Kept here so callers can validate a
/// role value before it ever reaches the database. What each role may
/// actually DO is Tenancy::OrgPerm::allows (src/tenancy/OrgPermissions.hpp),
/// not this function. Adding a role = migration (CHECK) + here + a column in
/// the OrgPermissions matrix (its static_asserts walk you through it).
inline bool is_valid_role(const std::string& role) {
    return role == "owner" || role == "member" || role == "viewer";
}

}  // namespace Tenancy
EOF
echo "==> Created $TENANCY/Organization.hpp"

# ── 2. src/tenancy/OrgMember.hpp ─────────────────────────────────────────
cat >"$TENANCY/OrgMember.hpp" <<'EOF'
/**
 * @file OrgMember.hpp
 * @brief Membership row: a user's role within an organization. Mirrors the
 *        `org_members` table. Installed by scripts/add-orgs.sh. role is
 *        CHECK-constrained to 'owner' | 'member' | 'viewer' at the DB layer;
 *        Tenancy::is_valid_role() (Organization.hpp) mirrors that app-side.
 */

#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace Tenancy {

struct OrgMember {
    std::string id;
    std::string org_id;
    std::string user_id;
    std::string role;  // 'owner' | 'member' | 'viewer'
    std::string created_at;
    std::string updated_at;

    template <typename Row>
    static OrgMember from_row(const Row& row) {
        OrgMember m;
        m.id = row["id"].template as<std::string>();
        m.org_id = row["org_id"].template as<std::string>();
        m.user_id = row["user_id"].template as<std::string>();
        m.role = row["role"].template as<std::string>();
        m.created_at = row["created_at"].template as<std::string>();
        m.updated_at = row["updated_at"].template as<std::string>();
        return m;
    }
};

inline void to_json(nlohmann::json& j, const OrgMember& m) {
    j = nlohmann::json{
        {"id", m.id},
        {"org_id", m.org_id},
        {"user_id", m.user_id},
        {"role", m.role},
        {"created_at", m.created_at},
        {"updated_at", m.updated_at},
    };
}

}  // namespace Tenancy
EOF
echo "==> Created $TENANCY/OrgMember.hpp"

# ── 3. src/tenancy/OrgScoped.hpp ─────────────────────────────────────────
cat >"$TENANCY/OrgScoped.hpp" <<'EOF'
/**
 * @file OrgScoped.hpp
 * @brief Header-only CRTP base for tenant-isolated reads — the org-scoped
 *        counterpart to Repositories::CrudBase's find_owned/list_owned/
 *        count_owned, but with NO unscoped find/list/count at all.
 *        Installed by scripts/add-orgs.sh.
 *
 * "A method that selects without an org does not exist" — every domain
 * repository that stores per-tenant rows inherits THIS base, not
 * Repositories::CrudBase, so a forgotten org filter is a compile error
 * (the global methods simply don't exist) rather than a runtime IDOR.
 * `new-resource.sh --org-scoped` scaffolds repositories onto this base.
 *
 * A derived repo provides the same four constants CrudBase expects, plus
 * kOrgColumn:
 *   class FooRepository : public Tenancy::OrgCrudBase<FooRepository, Domain::Foo, std::string> {
 *     public:
 *       static constexpr const char* kTable      = "foos";
 *       static constexpr const char* kColumns    = "id, org_id, name, created_at";
 *       static constexpr const char* kIdColumn   = "id";
 *       static constexpr const char* kOrderBy    = "created_at DESC";
 *       static constexpr const char* kOrgColumn  = "org_id";
 *       // ... bespoke create/update/remove (each carrying org_id) ...
 *   };
 *
 * KeyT is the id type (std::string for uuid PKs, int for serial PKs).
 * Deliberately no global find/list/count is offered — do not add them here;
 * a resource that genuinely needs a global read is not a fit for this base.
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "database/Database.hpp"

namespace Tenancy {

template <typename Derived, typename Entity, typename KeyT = std::string>
class OrgCrudBase {
public:
    /// Fetch by primary key, scoped to one organization. A row belonging to
    /// any other org is indistinguishable from a missing row (std::nullopt),
    /// which is the point — no separate 403 branch, no leak of existence.
    /// @p from_primary forces the primary (read-after-write).
    std::optional<Entity> find_in_org(const KeyT& id, const std::string& org_id, bool from_primary = false) {
        auto query = [&](auto& txn) -> std::optional<Entity> {
            auto r = txn.exec_params(
                select_prefix() + " WHERE " + Derived::kIdColumn + " = $1 AND " + Derived::kOrgColumn + " = $2",
                id,
                org_id);
            if (r.empty())
                return std::nullopt;
            return Entity::from_row(r[0]);
        };
        return from_primary ? Database::get().execute_read_primary(query) : Database::get().execute_read(query);
    }

    std::vector<Entity> list_in_org(const std::string& org_id, int limit = 100, int offset = 0) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(select_prefix() + " WHERE " + Derived::kOrgColumn + " = $1 ORDER BY " +
                                         Derived::kOrderBy + " LIMIT $2 OFFSET $3",
                                     org_id,
                                     limit,
                                     offset);
            std::vector<Entity> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(Entity::from_row(row));
            return out;
        });
    }

    long count_in_org(const std::string& org_id) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(
                "SELECT COUNT(*) FROM " + std::string(Derived::kTable) + " WHERE " + Derived::kOrgColumn + " = $1",
                org_id);
            return r.at(0).at(0).template as<long>();
        });
    }

private:
    static std::string select_prefix() {
        return "SELECT " + std::string(Derived::kColumns) + " FROM " + std::string(Derived::kTable);
    }
};

}  // namespace Tenancy
EOF
echo "==> Created $TENANCY/OrgScoped.hpp"

# ── 4. src/tenancy/OrganizationRepository.hpp ────────────────────────────
cat >"$TENANCY/OrganizationRepository.hpp" <<'EOF'
/**
 * @file OrganizationRepository.hpp
 * @brief All SQL touching `organizations` lives here. Installed by
 *        scripts/add-orgs.sh.
 *
 * find/list/count come from CrudBase (RoleRepository/AuditRepository style);
 * create/update_status are the bespoke queries this table needs. This is the
 * ONE org-side repository that legitimately extends the GLOBAL CrudBase:
 * organizations are the tenants themselves, listed by the platform admin —
 * everything inside a tenant goes through Tenancy::OrgCrudBase instead.
 */

#pragma once

#include <string>

#include <pqxx/pqxx>

#include "database/Database.hpp"
#include "repositories/CrudBase.hpp"
#include "repositories/RepoErrors.hpp"
#include "tenancy/Organization.hpp"

namespace Tenancy {

struct OrganizationNotFound : Repositories::NotFoundError {
    OrganizationNotFound() : Repositories::NotFoundError("organization") {}
};

class OrganizationRepository : public Repositories::CrudBase<OrganizationRepository, Organization, std::string> {
public:
    // CrudBase contract — supplies find(id) / list(limit,offset) / count().
    // The column list is EXPLICIT, not `*`: Organization::from_row reads each
    // field by name, so a column added by a migration but forgotten here
    // would break reads at runtime, not at build time.
    static constexpr const char* kTable = "organizations";
    static constexpr const char* kColumns = "id, name, status, created_at, updated_at";
    static constexpr const char* kIdColumn = "id";
    static constexpr const char* kOrderBy = "created_at DESC";

    /// Insert a new tenant. Name is deliberately NOT unique — two customers
    /// may share a company name; identity is the UUID.
    Organization create(const std::string& name) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params("INSERT INTO organizations (name) VALUES ($1) RETURNING " + std::string(kColumns),
                                     name);
            return Organization::from_row(r[0]);
        });
    }

    /**
     * @brief Transition status ('active' | 'suspended' | 'archived', see the
     *        CHECK constraint in the organizations migration).
     * @return false if no such organization — an expected branch the caller
     *         maps to 404, not an exceptional repo error.
     */
    bool update_status(const std::string& id, const std::string& status) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params("UPDATE organizations SET status = $2 WHERE id = $1 RETURNING id", id, status);
            return !r.empty();
        });
    }
};

}  // namespace Tenancy
EOF
echo "==> Created $TENANCY/OrganizationRepository.hpp"

# ── 5. src/tenancy/OrgMemberRepository.hpp ───────────────────────────────
cat >"$TENANCY/OrgMemberRepository.hpp" <<'EOF'
/**
 * @file OrgMemberRepository.hpp
 * @brief All SQL touching `org_members` lives here. Installed by
 *        scripts/add-orgs.sh.
 *
 * Every query is scoped by the (org_id, user_id) composite the table's
 * UNIQUE constraint enforces rather than the row's own `id`, so this repo
 * does NOT extend CrudBase (same rationale as UserRepository not extending
 * it: the bespoke access pattern doesn't fit the single-PK contract).
 * role is app-side validated via Tenancy::is_valid_role() where useful; the
 * DB CHECK constraint is the final backstop (raises 23514 if bypassed).
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <pqxx/pqxx>

#include <nlohmann/json.hpp>

#include "database/Database.hpp"
#include "repositories/RepoErrors.hpp"
#include "repositories/SqlErrors.hpp"
#include "tenancy/OrgMember.hpp"

namespace Tenancy {

// Stable 409 code carried on the exception, mirrors UserRepository's
// DuplicateEmail — Api::with_repo_errors maps it without including this file.
struct DuplicateMembership : Repositories::ConflictError {
    DuplicateMembership()
        : Repositories::ConflictError("membership_exists", "This user is already a member of the organization") {}
};

/**
 * @brief One roster row for `GET /orgs/{id}/members`: a member's user_id
 *        joined with the user's email, plus role/created_at. Deliberately
 *        NOT the full OrgMember shape — a bare user_id is useless for a UI
 *        roster (nowhere to look up a UUID's email), so this is a
 *        purpose-built read model for the list-with-email query below.
 */
struct MemberWithEmail {
    std::string user_id;
    std::string email;
    std::string role;  // 'owner' | 'member' | 'viewer'
    std::string created_at;

    template <typename Row>
    static MemberWithEmail from_row(const Row& row) {
        MemberWithEmail m;
        m.user_id = row["user_id"].template as<std::string>();
        m.email = row["email"].template as<std::string>();
        m.role = row["role"].template as<std::string>();
        m.created_at = row["created_at"].template as<std::string>();
        return m;
    }
};

inline void to_json(nlohmann::json& j, const MemberWithEmail& m) {
    j = nlohmann::json{
        {"user_id", m.user_id},
        {"email", m.email},
        {"role", m.role},
        {"created_at", m.created_at},
    };
}

class OrgMemberRepository {
public:
    static constexpr const char* kColumns = "id, org_id, user_id, role, created_at, updated_at";

    /// The membership row for one (org, user) pair, if any.
    std::optional<OrgMember> find_membership(const std::string& org_id, const std::string& user_id) {
        return Database::get().execute_read([&](auto& txn) -> std::optional<OrgMember> {
            auto r = txn.exec_params(
                "SELECT " + std::string(kColumns) + " FROM org_members WHERE org_id = $1 AND user_id = $2",
                org_id,
                user_id);
            if (r.empty())
                return std::nullopt;
            return OrgMember::from_row(r[0]);
        });
    }

    /// Every organization a user belongs to.
    std::vector<OrgMember> list_for_user(const std::string& user_id) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(
                "SELECT " + std::string(kColumns) + " FROM org_members WHERE user_id = $1 ORDER BY created_at",
                user_id);
            std::vector<OrgMember> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(OrgMember::from_row(row));
            return out;
        });
    }

    /// Every member of an organization.
    std::vector<OrgMember> list_members(const std::string& org_id) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(
                "SELECT " + std::string(kColumns) + " FROM org_members WHERE org_id = $1 ORDER BY created_at", org_id);
            std::vector<OrgMember> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(OrgMember::from_row(row));
            return out;
        });
    }

    /**
     * @brief Every member of an organization joined with the user's email —
     *        the roster `GET /orgs/{id}/members` renders. A hard JOIN (not
     *        LEFT JOIN) is correct: org_members.user_id is a FK into users,
     *        so a membership row with no matching user can't exist.
     */
    std::vector<MemberWithEmail> list_members_with_email(const std::string& org_id) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(
                "SELECT m.user_id, u.email, m.role, m.created_at "
                "FROM org_members m JOIN users u ON u.id = m.user_id "
                "WHERE m.org_id = $1 ORDER BY m.created_at",
                org_id);
            std::vector<MemberWithEmail> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(MemberWithEmail::from_row(row));
            return out;
        });
    }

    /**
     * @brief Add a user to an organization with a role. Throws
     *        DuplicateMembership on UNIQUE(org_id, user_id) violation
     *        (SQLSTATE 23505) — a user holds ONE role per org.
     */
    OrgMember add(const std::string& org_id, const std::string& user_id, const std::string& role) {
        return Repositories::detail::translate_sql(
            [&] {
                return Database::get().execute_write([&](auto& txn) {
                    auto r = txn.exec_params(
                        "INSERT INTO org_members (org_id, user_id, role) VALUES ($1, $2, $3) "
                        "RETURNING " +
                            std::string(kColumns),
                        org_id,
                        user_id,
                        role);
                    return OrgMember::from_row(r[0]);
                });
            },
            Repositories::detail::throw_on<DuplicateMembership>("23505"));
    }

    /// @return false if no such membership — an expected branch the caller
    ///         maps to 404, not an exceptional repo error.
    bool set_role(const std::string& org_id, const std::string& user_id, const std::string& role) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params("UPDATE org_members SET role = $3 WHERE org_id = $1 AND user_id = $2 RETURNING id",
                                     org_id,
                                     user_id,
                                     role);
            return !r.empty();
        });
    }

    /// @return false if no such membership existed.
    bool remove(const std::string& org_id, const std::string& user_id) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "DELETE FROM org_members WHERE org_id = $1 AND user_id = $2 RETURNING id", org_id, user_id);
            return !r.empty();
        });
    }
};

}  // namespace Tenancy
EOF
echo "==> Created $TENANCY/OrgMemberRepository.hpp"

# ── 6. src/tenancy/OrgContext.hpp ────────────────────────────────────────
cat >"$TENANCY/OrgContext.hpp" <<'EOF'
/**
 * @file OrgContext.hpp
 * @brief Resolves the caller's organization context (org id + role) from the
 *        access-token `org` claim, fail-closed against a LIVE membership row.
 *        Installed by scripts/add-orgs.sh.
 * @details The `org` claim is minted at login/refresh (AuthController) only
 *          when the user belongs to exactly one organization — with zero or
 *          more than one membership no claim is set and the client must pick
 *          via POST /orgs/{id}/switch. Guard consumers use API_REQUIRE_ORG
 *          (src/api/Guards.hpp), not this function directly.
 */

#pragma once

#include <optional>
#include <string>

#include <drogon/HttpRequest.h>

#include "security/Auth.hpp"
#include "tenancy/OrgMemberRepository.hpp"

namespace Tenancy {

struct OrgContext {
    std::string org_id;
    std::string role;  // 'owner' | 'member' | 'viewer'
    std::string user_id;
};

/**
 * @brief Fail-closed: a context exists only when the principal carries a
 *        non-empty `org` claim AND a live membership row backs it up. No
 *        principal, no claim, or a revoked/nonexistent membership all yield
 *        nullopt — callers must treat that as "no org access", never as
 *        "unscoped access". The per-request membership read is what makes
 *        removal from an org take effect IMMEDIATELY, not at token expiry —
 *        add caching here only if it shows up in profiles, and only with a
 *        TTL you can defend as a revocation delay.
 */
inline std::optional<OrgContext> org_context_of(const drogon::HttpRequestPtr& req) {
    auto p = Security::Auth::principal_of(req);
    if (!p || p->subject.empty() || p->org.empty())
        return std::nullopt;
    OrgMemberRepository members;
    auto m = members.find_membership(p->org, p->subject);
    if (!m)
        return std::nullopt;
    return OrgContext{p->org, m->role, p->subject};
}

}  // namespace Tenancy
EOF
echo "==> Created $TENANCY/OrgContext.hpp"

# ── 7. src/tenancy/OrgPermissions.hpp ────────────────────────────────────
cat >"$TENANCY/OrgPermissions.hpp" <<'EOF'
/**
 * @file OrgPermissions.hpp
 * @brief Tenant-role permission matrix as a pure function — DENY BY DEFAULT.
 *        Installed by scripts/add-orgs.sh; EDIT the matrix for your domain.
 * @details The alternative — a denylist like `if (ctx.role == "viewer")`
 *          scattered across controllers — fails open: any newly added role
 *          walks straight through it into full CRUD (this happened downstream;
 *          this table is the fix). Here the grants are written explicitly and
 *          an unknown role, unknown resource, unfilled cell and unknown
 *          action all produce false.
 *
 *          An empty grant ("") means INVISIBLE, not read-only: gate reads as
 *          well as writes (API_REQUIRE_ORG_PERM on every GET too).
 *
 *          Roles are listed once, in detail::kRoles; the grants in each
 *          kMatrix row are POSITIONAL over that list. A role added to kRoles
 *          but missing from a resource row gets nullptr — i.e. denial. That
 *          is the property this table exists for: a forgotten cell closes
 *          access instead of opening it.
 *
 *          Pure module: no DB, no Drogon — unit-tested without infra
 *          (tests/unit/test_org_permissions.cpp). Consumers use the
 *          API_REQUIRE_ORG_PERM macro (src/api/Guards.hpp), not allows().
 *
 *          TODO after install: replace the example `widgets` row with your
 *          real resources (one row per org-scoped resource; `new-resource.sh
 *          --org-scoped` generates guards keyed by the plural table name),
 *          and mirror every row in kAllResources in the unit test.
 */

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace Tenancy::OrgPerm {

namespace Resource {
/// EXAMPLE resource — matches what `new-resource.sh Widget --org-scoped`
/// generates. Replace with your domain's resources.
inline constexpr const char* kWidgets = "widgets";
/// Membership management. Kept owner-only below; note the Organizations
/// controller's member routes are gated by require_admin_or_org_owner
/// directly (same rule, plus the platform-admin fallback the tenant-role
/// matrix cannot express) — this row documents the policy for any OTHER
/// surface that exposes membership data.
inline constexpr const char* kMembers = "members";
}  // namespace Resource

namespace Action {
inline constexpr const char* kRead = "read";
inline constexpr const char* kWrite = "write";
}  // namespace Action

namespace detail {

/// The matrix columns, in order. A role not listed here owns no cell at all —
/// so it is denied everything.
inline constexpr const char* kRoles[] = {"owner", "member", "viewer"};
inline constexpr std::size_t kRoleCount = sizeof(kRoles) / sizeof(kRoles[0]);

/// One matrix row. @c grants are POSITIONAL over kRoles. Grant values:
/// "rw" — read and write, "r" — read only, "" — invisible (no read, no
/// write), nullptr — cell not filled in at all, which also means denial.
struct MatrixRow {
    const char* resource;
    const char* grants[kRoleCount];
};

/// The permission matrix. Column order — kRoles. Columns are aligned by hand
/// (clang-format off): this table is read by eye in review, and a misaligned
/// column costs more here than formatting uniformity.
// clang-format off
inline constexpr MatrixRow kMatrix[] = {
    //  resource              owner  member  viewer
    {Resource::kWidgets,     {"rw",   "rw",   "r"}},
    {Resource::kMembers,     {"rw",    "",    ""}},
};
// clang-format on

/// Index of a role in kRoles, or kRoleCount if the role is unknown.
constexpr std::size_t role_index(std::string_view role) {
    for (std::size_t i = 0; i < kRoleCount; ++i) {
        if (role == kRoles[i])
            return i;
    }
    return kRoleCount;
}

/**
 * @brief The grant @p role holds on @p resource in table @p rows.
 * @return The grant string ("rw" / "r" / ""), or nullptr when the pair is
 *         not in the table: unknown role, unknown resource, or an unfilled
 *         cell. Templated over the row array (not hardwired to kMatrix) so
 *         the unit test can run the same logic over its own table with a
 *         deliberately missing cell and prove it denies.
 */
template <std::size_t N>
constexpr const char* grant_for(const MatrixRow (&rows)[N], std::string_view role, std::string_view resource) {
    const std::size_t r = role_index(role);
    if (r == kRoleCount)
        return nullptr;  // unknown role
    for (std::size_t i = 0; i < N; ++i) {
        if (resource == rows[i].resource)
            return rows[i].grants[r];  // may be nullptr — unfilled cell
    }
    return nullptr;  // unknown resource
}

/// Each role's position in kRoles, pinned at compile time. A skipped cell in
/// a kMatrix row is structurally safe (nullptr = denial), but a REORDERED
/// kRoles silently shifts every grant one column over — a member would get
/// the owner column. These checks break the build in the file being edited:
/// append new roles to the END of kRoles, add their static_assert here, and
/// add their column to every kMatrix row.
static_assert(kRoleCount == 3, "role added — add its static_assert below and a column to every kMatrix row");
static_assert(role_index("owner") == 0, "kRoles order changed — kMatrix grants shifted");
static_assert(role_index("member") == 1, "kRoles order changed — kMatrix grants shifted");
static_assert(role_index("viewer") == 2, "kRoles order changed — kMatrix grants shifted");
static_assert(role_index("nope") == kRoleCount, "an unknown role must own no column at all");

}  // namespace detail

/**
 * @brief Whether @p role may perform @p action on @p resource.
 * @details Deny-by-default in all three dimensions: unknown resource,
 *          unknown role and unknown action all give false. Any non-empty
 *          grant implies read ("rw" and "r" read; "" and nullptr don't);
 *          write requires the letter 'w'.
 */
inline bool allows(const std::string& role, const std::string& resource, const std::string& action) {
    const char* grants = detail::grant_for(detail::kMatrix, role, resource);
    if (grants == nullptr)
        return false;
    const std::string_view granted(grants);
    if (action == Action::kRead)
        return !granted.empty();
    if (action == Action::kWrite)
        return granted.find('w') != std::string_view::npos;
    return false;  // unknown action
}

}  // namespace Tenancy::OrgPerm
EOF
echo "==> Created $TENANCY/OrgPermissions.hpp"

# ── 8. src/api/OrganizationsController.hpp ───────────────────────────────
cat >"$ORGS_CTRL" <<'EOF'
/**
 * @file OrganizationsController.hpp
 * @brief Organizations (tenants) + membership management API. Installed by
 *        scripts/add-orgs.sh. Tenants are created by a platform admin, not
 *        by self-service signup — wire your own signup flow if you need one.
 *
 * Routes (all under /api/v1):
 *   POST   /api/v1/orgs                          create a tenant (admin)
 *   GET    /api/v1/orgs                          list tenants (admin, paginated)
 *   GET    /api/v1/orgs/mine                     the caller's orgs + their role
 *   POST   /api/v1/orgs/{id}/switch              mint an access token scoped to {id}
 *   GET    /api/v1/orgs/{id}/members             list members + email (admin or owner)
 *   POST   /api/v1/orgs/{id}/members             add a member (admin or owner)
 *   PATCH  /api/v1/orgs/{id}/members/{user_id}   change a member's role (admin or owner)
 *   DELETE /api/v1/orgs/{id}/members/{user_id}   remove a member (admin or owner)
 *
 * Member-management gate ("admin or owner-of-this-org"): tries the caller's
 * org context first (the access token's `org` claim must already be {id} AND
 * the backing membership role must be "owner" — see
 * require_admin_or_org_owner() below), and falls back to full-admin
 * otherwise. Fail-closed: a caller who IS owner of {id} but hasn't switched
 * into it (org claim points elsewhere, or is empty) is NOT authorized here —
 * only as admin, if they happen to also be one.
 *
 * `members` in the OrgPermissions matrix is owner-only; that is already
 * enforced by require_admin_or_org_owner, so an extra API_REQUIRE_ORG_PERM
 * on these routes would be a second source of truth saying the same thing.
 */

#pragma once

#include <optional>
#include <string>

#include <drogon/HttpController.h>
#include <drogon/drogon.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/HandlerSupport.hpp"
#include "api/RequestUtils.hpp"
#include "api/Validation.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Audit.hpp"
#include "security/Auth.hpp"
#include "tenancy/OrgContext.hpp"
#include "tenancy/OrgMember.hpp"
#include "tenancy/OrgMemberRepository.hpp"
#include "tenancy/Organization.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "utils/ErrorResponse.hpp"
#include "utils/Time.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class OrganizationsController : public HttpController<OrganizationsController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(OrganizationsController::createOrganization, "/api/v1/orgs", Post);
    ADD_METHOD_TO(OrganizationsController::listOrganizations, "/api/v1/orgs", Get);
    ADD_METHOD_TO(OrganizationsController::mine, "/api/v1/orgs/mine", Get);
    ADD_METHOD_TO(OrganizationsController::switchOrg, "/api/v1/orgs/{1}/switch", Post);
    ADD_METHOD_TO(OrganizationsController::listMembers, "/api/v1/orgs/{1}/members", Get);
    ADD_METHOD_TO(OrganizationsController::addMember, "/api/v1/orgs/{1}/members", Post);
    ADD_METHOD_TO(OrganizationsController::updateMemberRole, "/api/v1/orgs/{1}/members/{2}", Patch);
    ADD_METHOD_TO(OrganizationsController::removeMember, "/api/v1/orgs/{1}/members/{2}", Delete);
    METHOD_LIST_END

    // ---------------------------------------------------------------------
    // POST /api/v1/orgs — { name }. Admin-gated: tenants are provisioned by
    // a platform admin. The caller (when authenticated) is bound into
    // org_members as "owner" right after create — best-effort, see inline —
    // so the admin who provisions a tenant isn't left outside it, unable to
    // reach the owner-gated member-management routes for their own org.
    // ---------------------------------------------------------------------
    void createOrganization(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ADMIN(req, callback);
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Validation::Errors errs;
        Validation::require(errs, body, "name");
        Validation::string_length(errs, body, "name", 1, 255);
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        with_repo_errors(callback, "orgs createOrganization", [&] {
            Tenancy::OrganizationRepository orgs;
            auto created = orgs.create(body["name"].get<std::string>());
            Security::Audit::record(actor_of(req), "org.create", "organization", created.id, {{"name", created.name}});

            // Bind the creator as owner. Best-effort: org.create already
            // committed, so a failure here (a concurrent add() race, a
            // transient DB error) must not turn an already-real organization
            // into a 500 the client would just retry into a duplicate.
            // Logged so an operator can backfill the membership by hand; the
            // 201 still carries the organization either way. principal_of()
            // is nullopt under AUTH_MODE=none — the org is then created
            // unowned (a membership row is meaningless without an identity).
            if (auto principal = Security::Auth::principal_of(req)) {
                try {
                    Tenancy::OrgMemberRepository members;
                    auto membership = members.add(created.id, principal->subject, "owner");
                    Security::Audit::record(actor_of(req),
                                            "org.member.add",
                                            "organization",
                                            created.id,
                                            {{"user_id", membership.user_id}, {"role", membership.role}});
                } catch (const std::exception& e) {
                    spdlog::error("orgs createOrganization: failed to bind creator {} as owner of org {}: {}",
                                  principal->subject,
                                  created.id,
                                  e.what());
                }
            }

            callback(Response::created({{"data", json(created)}}));
        });
    }

    // ---------------------------------------------------------------------
    // GET /api/v1/orgs — admin-gated, paginated (same limit/offset contract
    // as AdminController::listUsers).
    // ---------------------------------------------------------------------
    void listOrganizations(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ADMIN(req, callback);
        const auto page = parse_page_params(req, /*default_limit=*/50, /*max_limit=*/200);

        with_repo_errors(callback, "orgs listOrganizations", [&] {
            Tenancy::OrganizationRepository repo;
            auto orgs = repo.list(page.limit, page.offset);
            long total = repo.count();
            callback(Response::paginated(to_json_array(orgs), total, page.limit, page.offset));
        });
    }

    // ---------------------------------------------------------------------
    // GET /api/v1/orgs/mine — any authenticated user; the organizations they
    // belong to, each annotated with their role in it.
    // ---------------------------------------------------------------------
    void mine(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_PRINCIPAL(req, callback, principal);

        with_repo_errors(callback, "orgs mine", [&] {
            Tenancy::OrgMemberRepository members;
            auto memberships = members.list_for_user(principal->subject);
            Tenancy::OrganizationRepository orgs;
            json data = json::array();
            for (const auto& m : memberships) {
                auto org = orgs.find(m.org_id);
                if (!org)
                    continue;  // FK guarantees this can't happen; stay defensive anyway
                json item = json(*org);
                item["role"] = m.role;
                data.push_back(item);
            }
            callback(Response::list(data));
        });
    }

    // ---------------------------------------------------------------------
    // POST /api/v1/orgs/{id}/switch
    //
    // Any authenticated member of {id} gets a freshly-minted access token
    // whose claims are IDENTICAL in shape to AuthController::mint_session's
    // access token, plus `org: {id}`. This is a deliberately SEPARATE mint
    // path, not a call into mint_session — mint_session's single-membership
    // heuristic decides the org claim for login/refresh only; here the org
    // is explicit and membership-checked against the path id. The refresh
    // token/cookie is never touched by this endpoint.
    //
    // Consequence the frontend must handle (docs/ORGS.md): after the next
    // POST /api/v1/auth/refresh the active org RESETS to mint_session's
    // single-membership default — a client with multiple organizations must
    // repeat /switch after every refresh to stay scoped to a non-default org.
    // ---------------------------------------------------------------------
    void switchOrg(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback,
                   const std::string& id) {
        API_REQUIRE_PRINCIPAL(req, callback, principal);
        if (!require_valid_uuid(id, callback))
            return;

        with_repo_errors(callback, "orgs switchOrg", [&] {
            Tenancy::OrgMemberRepository members;
            if (!members.find_membership(id, principal->subject)) {
                callback(ErrorResponse::forbidden("not_a_member", "You are not a member of this organization"));
                return;
            }

            Repositories::UserRepository users;
            auto user = users.find(principal->subject);
            if (!user) {
                callback(ErrorResponse::unauthorized("user_gone"));
                return;
            }

            const auto& cfg = Security::Auth::get().config();
            if (cfg.jwt_secret.empty()) {
                spdlog::error("orgs switchOrg: JWT_SECRET unset");
                callback(ErrorResponse::service_unavailable("session_unavailable", "Could not mint session"));
                return;
            }
            const long now = Utils::Time::now_epoch_seconds();

            json roles_array = json::array();
            if (user->role)
                roles_array.push_back(user->role->name);
            const std::uint32_t perm_bits = user->role ? user->role->permissions : 0u;

            // Same claim set as AuthController::mint_session's access token
            // (see the method comment above for why this isn't a shared call).
            json access_claims = {
                {"sub", user->id},
                {"iat", now},
                {"exp", now + cfg.cookies.access_ttl_sec},
                {"typ", "access"},
                {"confirmed", user->confirmed},
                {"permissions", perm_bits},
                {cfg.jwt_roles_claim, roles_array},
                {"org", id},
            };
            if (!cfg.jwt_issuer.empty())
                access_claims["iss"] = cfg.jwt_issuer;
            if (!cfg.jwt_audience.empty())
                access_claims["aud"] = cfg.jwt_audience;

            const std::string access = Security::Auth::issue_hs256_jwt(access_claims, cfg.jwt_secret);
            Security::Audit::record(actor_of(req), "org.switch", "organization", id);

            // Cookie-mode clients (SPA) read the access token from the
            // HttpOnly access cookie, never from the JSON body — without
            // this, a browser session under cookies.enabled=true would keep
            // sending the OLD access cookie and the switch would silently
            // not take effect. Empty refresh_token leaves the refresh cookie
            // untouched (set_session_cookies only overwrites it when
            // non-empty). The JSON body still carries `access` for
            // Bearer-mode clients and tests.
            auto http = Response::ok({{"access", access}});
            Security::Auth::set_session_cookies(http, cfg.cookies, access, "");
            callback(http);
        });
    }

    // ---------------------------------------------------------------------
    // GET /api/v1/orgs/{id}/members — the roster (admin, or owner of this
    // organization), joined with each member's email so the frontend has
    // something a human can act on instead of a bare user_id.
    // ---------------------------------------------------------------------
    void listMembers(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback,
                     const std::string& id) {
        if (auto err = require_admin_or_org_owner(req, id)) {
            callback(err);
            return;
        }
        if (!require_valid_uuid(id, callback))
            return;

        with_repo_errors(callback, "orgs listMembers", [&] {
            Tenancy::OrgMemberRepository members;
            callback(Response::list(to_json_array(members.list_members_with_email(id))));
        });
    }

    // ---------------------------------------------------------------------
    // POST /api/v1/orgs/{id}/members — { user_id, role } OR { email, role }.
    //
    // The email variant exists because the "add a member" UI has no way to
    // discover a user's raw UUID — it resolves the email via
    // UserRepository::find_by_email first, then continues through the exact
    // same add() path (same audit event, same DuplicateMembership 409).
    // Both identifying fields at once is a 400 (ambiguous request shape).
    // ---------------------------------------------------------------------
    void addMember(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback,
                   const std::string& id) {
        if (auto err = require_admin_or_org_owner(req, id)) {
            callback(err);
            return;
        }
        if (!require_valid_uuid(id, callback))
            return;
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        const bool has_user_id = body.contains("user_id") && !body["user_id"].is_null();
        const bool has_email = body.contains("email") && !body["email"].is_null();

        Validation::Errors errs;
        if (has_user_id && has_email) {
            errs.add("email", "mutually_exclusive", "provide either user_id or email, not both");
            callback(Validation::response_400(errs));
            return;
        }

        Validation::require(errs, body, "role");
        if (has_email) {
            Validation::email(errs, body, "email");
        } else {
            Validation::require(errs, body, "user_id");
            Validation::uuid(errs, body, "user_id");
        }
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }
        if (!body["role"].is_string() || !Tenancy::is_valid_role(body["role"].get<std::string>())) {
            Validation::Errors role_errs;
            role_errs.add("role", "not_allowed", "must be one of: 'owner' 'member' 'viewer'");
            callback(Validation::response_400(role_errs));
            return;
        }

        std::string user_id;
        if (has_email) {
            Repositories::UserRepository users;
            auto found = users.find_by_email(body["email"].get<std::string>());
            if (!found) {
                callback(ErrorResponse::make({drogon::k404NotFound,
                                              "user_not_found",
                                              "No user with this email — ask them to register first",
                                              json::object()}));
                return;
            }
            user_id = found->id;
        } else {
            user_id = body["user_id"].get<std::string>();
        }

        with_repo_errors(callback, "orgs addMember", [&] {
            Tenancy::OrgMemberRepository members;
            auto created = members.add(id, user_id, body["role"].get<std::string>());
            Security::Audit::record(actor_of(req),
                                    "org.member.add",
                                    "organization",
                                    id,
                                    {{"user_id", created.user_id}, {"role", created.role}});
            callback(Response::created({{"data", json(created)}}));
        });
    }

    // ---------------------------------------------------------------------
    // PATCH /api/v1/orgs/{id}/members/{user_id} — { role }.
    //
    // Last-owner protection: demoting the organization's sole owner is a
    // 409, counted via a fresh list_members() scan right before the write.
    // Race window: this count-then-mutate is NOT one transaction —
    // OrgMemberRepository gives each call its own transaction, so two
    // concurrent demotions of two different "last" owners of a two-owner org
    // could both observe count()==2 and both succeed, leaving zero owners.
    // Closing this fully needs a single SQL statement with a subquery guard;
    // known gap, keep it in mind before you ship member self-service.
    // ---------------------------------------------------------------------
    void updateMemberRole(const HttpRequestPtr& req,
                          std::function<void(const HttpResponsePtr&)>&& callback,
                          const std::string& id,
                          const std::string& user_id) {
        if (auto err = require_admin_or_org_owner(req, id)) {
            callback(err);
            return;
        }
        if (!require_valid_uuid(id, callback) || !require_valid_uuid(user_id, callback))
            return;
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Validation::Errors errs;
        Validation::require(errs, body, "role");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }
        if (!body["role"].is_string() || !Tenancy::is_valid_role(body["role"].get<std::string>())) {
            Validation::Errors role_errs;
            role_errs.add("role", "not_allowed", "must be one of: 'owner' 'member' 'viewer'");
            callback(Validation::response_400(role_errs));
            return;
        }
        const std::string new_role = body["role"].get<std::string>();

        with_repo_errors(callback, "orgs updateMemberRole", [&] {
            Tenancy::OrgMemberRepository members;
            auto target = members.find_membership(id, user_id);
            if (!target) {
                callback(ErrorResponse::not_found("member"));
                return;
            }
            if (target->role == "owner" && new_role != "owner" && count_owners(id) <= 1) {
                callback(ErrorResponse::conflict("last_owner", "Cannot demote the last owner of an organization"));
                return;
            }
            members.set_role(id, user_id, new_role);
            auto fresh = members.find_membership(id, user_id);
            Security::Audit::record(actor_of(req),
                                    "org.member.role_change",
                                    "organization",
                                    id,
                                    {{"user_id", user_id}, {"role", new_role}});
            callback(Response::ok({{"data", json(*fresh)}}));
        });
    }

    // ---------------------------------------------------------------------
    // DELETE /api/v1/orgs/{id}/members/{user_id}
    //
    // Last-owner protection — same race-window caveat as updateMemberRole.
    // ---------------------------------------------------------------------
    void removeMember(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback,
                      const std::string& id,
                      const std::string& user_id) {
        if (auto err = require_admin_or_org_owner(req, id)) {
            callback(err);
            return;
        }
        if (!require_valid_uuid(id, callback) || !require_valid_uuid(user_id, callback))
            return;

        with_repo_errors(callback, "orgs removeMember", [&] {
            Tenancy::OrgMemberRepository members;
            auto target = members.find_membership(id, user_id);
            if (!target) {
                callback(ErrorResponse::not_found("member"));
                return;
            }
            if (target->role == "owner" && count_owners(id) <= 1) {
                callback(ErrorResponse::conflict("last_owner", "Cannot remove the last owner of an organization"));
                return;
            }
            members.remove(id, user_id);
            Security::Audit::record(actor_of(req), "org.member.remove", "organization", id, {{"user_id", user_id}});
            callback(Response::ok({{"message", "Member removed"}}));
        });
    }

private:
    /// Acting principal's subject for the audit trail ("" when auth off).
    static std::string actor_of(const HttpRequestPtr& req) {
        auto p = Security::Auth::principal_of(req);
        return p ? p->subject : std::string{};
    }

    /// Member-management gate: the caller's org context must already point
    /// at {org_id} with role "owner" (see the file-level comment), else fall
    /// back to full-admin. Returns nullptr when authorized, an error
    /// response otherwise — same contract as Security::Auth::require_admin.
    static drogon::HttpResponsePtr require_admin_or_org_owner(const HttpRequestPtr& req, const std::string& org_id) {
        auto ctx = Tenancy::org_context_of(req);
        if (ctx && ctx->org_id == org_id && ctx->role == "owner")
            return {};
        return Security::Auth::require_admin(req);
    }

    /// Count of members holding "owner" in {org_id} — used by the
    /// last-owner protections above. A plain read, not part of the caller's
    /// write transaction (see the race-window comment on updateMemberRole).
    static int count_owners(const std::string& org_id) {
        Tenancy::OrgMemberRepository members;
        int count = 0;
        for (const auto& m : members.list_members(org_id)) {
            if (m.role == "owner")
                ++count;
        }
        return count;
    }
};

}  // namespace Api
EOF
echo "==> Created $ORGS_CTRL"

# ── 9. Patch src/api/Guards.hpp — org guard macros ───────────────────────
cat >"$TMP/guards_includes.frag" <<'EOF'
#include "tenancy/OrgContext.hpp"
#include "tenancy/OrgPermissions.hpp"
EOF
insert_after "$GUARDS" '#include "security/Auth.hpp"' "$TMP/guards_includes.frag"

cat >"$TMP/guards_macros.frag" <<'EOF'
/// Bind the caller's organization context (org id + tenant role) into `ctx`
/// (Tenancy::OrgContext), or reject with 403. Fail-closed like
/// API_REQUIRE_OWNER — deliberately NOT a no-op when auth is disabled:
/// tenant-scoped data is meaningless without an identity + a live membership
/// row backing the `org` claim, so org-scoped endpoints genuinely require
/// AUTH_MODE != none (a "disabled" org gate would be an illusion of
/// isolation, not a dev convenience). Installed by scripts/add-orgs.sh.
#define API_REQUIRE_ORG(req, callback, ctx)           \
    Tenancy::OrgContext ctx;                          \
    do {                                              \
        auto _org_ctx = Tenancy::org_context_of(req); \
        if (!_org_ctx) {                              \
            callback(ErrorResponse::forbidden());     \
            return;                                   \
        }                                             \
        (ctx) = *_org_ctx;                            \
    } while (0)

/// Reject with 403 unless @p ctx's TENANT role is granted (RES, ACT) by the
/// matrix (Tenancy::OrgPerm::allows). Deny-by-default: an unknown role, an
/// unknown resource and an unknown action all produce a 403 — the safe
/// failure mode when a resource row hasn't been added to the matrix yet.
///
/// Takes no `req`: everything this decision needs already sits in the
/// OrgContext bound by API_REQUIRE_ORG (same shape as API_REQUIRE_JOBS_READY,
/// which also takes only the callback). Use it AFTER API_REQUIRE_ORG in
/// every org-scoped handler, on reads as well as writes: an empty grant in
/// the matrix means invisible, not read-only.
#define API_REQUIRE_ORG_PERM(callback, ctx, RES, ACT)                                                              \
    do {                                                                                                           \
        if (!Tenancy::OrgPerm::allows((ctx).role, (RES), (ACT))) {                                                 \
            callback(ErrorResponse::forbidden(                                                                     \
                "org_role_denied",                                                                                 \
                "Your role in this organization is not allowed to " + std::string(ACT) + " " + std::string(RES))); \
            return;                                                                                                \
        }                                                                                                          \
    } while (0)

EOF
insert_before "$GUARDS" 'namespace Api {' "$TMP/guards_macros.frag"
grep -q "API_REQUIRE_ORG_PERM" "$GUARDS" || die "failed to insert org macros into $GUARDS"
echo "==> Patched $GUARDS (API_REQUIRE_ORG / API_REQUIRE_ORG_PERM)"

# ── 10. Patch src/api/Api.hpp — controller include ───────────────────────
if ! grep -q '#include "api/OrganizationsController.hpp"' "$API_HPP"; then
    printf '#include "api/OrganizationsController.hpp"\n' >"$TMP/api_include.frag"
    insert_after "$API_HPP" '#include "api/Middleware.hpp"' "$TMP/api_include.frag"
    grep -q '#include "api/OrganizationsController.hpp"' "$API_HPP" || die "failed to add the include to $API_HPP"
    echo "==> Patched $API_HPP (#include OrganizationsController)"
fi

# ── 11. Patch src/api/AuthController.cpp — org claim + org_role in /me ───
grep -q "tenancy/OrgContext.hpp" "$AUTH_CTRL" && die "$AUTH_CTRL already references tenancy — partial install?"

cat >"$TMP/authctrl_includes.frag" <<'EOF'
#include "tenancy/OrgContext.hpp"
#include "tenancy/OrgMemberRepository.hpp"
EOF
insert_after "$AUTH_CTRL" '#include "security/SessionStore.hpp"' "$TMP/authctrl_includes.frag"

# mint_session: default the org claim when the user belongs to EXACTLY one
# organization. Anchored on the issuer line that closes the access-claims
# build; AuthController has exactly one (switchOrg's copy lives in
# OrganizationsController).
[[ "$(grep -cxF '    if (!cfg.jwt_issuer.empty())' "$AUTH_CTRL")" == "1" ]] ||
    die "expected exactly one issuer anchor line in $AUTH_CTRL — patch mint_session by hand (see docs/ORGS.md)"
cat >"$TMP/authctrl_mint.frag" <<'EOF'
    // Org claim: set ONLY when the user belongs to exactly one
    // organization — an unambiguous default. Zero memberships means
    // nothing to default to; more than one means the client must pick
    // explicitly via POST /orgs/{id}/switch. This same rule runs on BOTH
    // login and refresh (mint_session is the sole minting path for
    // both), so the claim is recomputed from current membership state
    // every time — it self-heals a stale claim (the user's one org was
    // deleted, a second org was added) instead of carrying forward a
    // value that may no longer be valid. verify_jwt() never requires
    // this claim, so tokens minted before add-orgs keep working.
    Tenancy::OrgMemberRepository org_members;
    auto memberships = org_members.list_for_user(user.id);
    if (memberships.size() == 1) {
        access_claims["org"] = memberships.front().org_id;
    }
EOF
insert_before "$AUTH_CTRL" '    if (!cfg.jwt_issuer.empty())' "$TMP/authctrl_mint.frag"
grep -q 'access_claims\["org"\]' "$AUTH_CTRL" || die "failed to patch mint_session in $AUTH_CTRL"

# /auth/me: annotate with the TENANT role (org_members.role), not the system
# role — the SPA needs it to hide org-scoped sections. Replaces the plain
# callback line; null when the token has no org claim or the membership was
# revoked (same fail-closed as org_context_of).
ME_ANCHOR='    callback(Response::ok({{"user", *user}}));'
[[ "$(grep -cxF "$ME_ANCHOR" "$AUTH_CTRL")" == "1" ]] ||
    die "expected exactly one '/auth/me' response line in $AUTH_CTRL — patch me() by hand (see docs/ORGS.md)"
cat >"$TMP/authctrl_me.frag" <<'EOF'
    // Tenant role (org_members.role), not the system role: the SPA needs
    // it to hide org-scoped sections. null when the token has no org
    // claim or the membership was revoked — the same fail-closed rule as
    // Tenancy::org_context_of.
    auto org_ctx = Tenancy::org_context_of(req);
    callback(Response::ok({{"user", *user}, {"org_role", org_ctx ? json(org_ctx->role) : json(nullptr)}}));
EOF
awk -v anchor="$ME_ANCHOR" -v fragment="$TMP/authctrl_me.frag" '
    $0 == anchor && !done {
        while ((getline l < fragment) > 0) print l
        close(fragment)
        done = 1
        next
    }
    { print }
' "$AUTH_CTRL" >"$AUTH_CTRL.tmp" && mv "$AUTH_CTRL.tmp" "$AUTH_CTRL"
grep -q 'org_role' "$AUTH_CTRL" || die "failed to patch me() in $AUTH_CTRL"
echo "==> Patched $AUTH_CTRL (org claim mint + org_role in /auth/me)"

# ── 12. Migration — next free NNN ────────────────────────────────────────
LAST=$(find "$ROOT/migrations" -maxdepth 1 -type f -name '[0-9][0-9][0-9]_*.sql' |
    sed -E 's|.*/([0-9]{3})_.*|\1|' | sort -n | tail -1 || true)
[[ -n "$LAST" ]] || die "no existing migrations found under $ROOT/migrations"
MIG_NUM=$(printf '%03d' $((10#$LAST + 1)))
MIG_FILE="$ROOT/migrations/${MIG_NUM}_organizations.sql"
[[ -e "$MIG_FILE" ]] && die "$MIG_FILE already exists"
cat >"$MIG_FILE" <<EOF
-- Migration ${MIG_NUM}: organizations + org_members — multi-tenancy foundation
-- (installed by scripts/add-orgs.sh). Every org-scoped domain table from here
-- on carries org_id UUID NOT NULL REFERENCES organizations(id); this migration
-- lays down the tenant table itself plus the membership/role join table that
-- scopes users to tenants.
--
-- updated_at mechanism matches migrations/000_updated_at_trigger.sql (the
-- shared touch_updated_at() function + a per-table trigger). users.id is UUID
-- (migrations/001_users_and_roles.sql), so org_members.user_id matches.
--
-- NOTE: MigrationRunner wraps this file in ONE transaction (under an advisory
-- lock) and records schema_migrations in that same transaction. Do NOT add
-- BEGIN/COMMIT here.

-- organizations: tenants of the system.
CREATE TABLE IF NOT EXISTS organizations (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name        TEXT NOT NULL,
    status      TEXT NOT NULL DEFAULT 'active'
                CHECK (status IN ('active', 'suspended', 'archived')),
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

DROP TRIGGER IF EXISTS organizations_touch_updated_at ON organizations;
CREATE TRIGGER organizations_touch_updated_at
    BEFORE UPDATE ON organizations
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();

-- org_members: membership of a user in an organization, with a tenant role.
-- The CHECK list must stay in sync with Tenancy::is_valid_role()
-- (src/tenancy/Organization.hpp) and the OrgPermissions matrix.
CREATE TABLE IF NOT EXISTS org_members (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id      UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    user_id     UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    role        TEXT NOT NULL CHECK (role IN ('owner', 'member', 'viewer')),
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (org_id, user_id)
);

DROP TRIGGER IF EXISTS org_members_touch_updated_at ON org_members;
CREATE TRIGGER org_members_touch_updated_at
    BEFORE UPDATE ON org_members
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();

CREATE INDEX IF NOT EXISTS idx_org_members_user ON org_members (user_id);
EOF
echo "==> Created $MIG_FILE"

# ── 13. Routes into get_endpoints() (Endpoints.hpp) ──────────────────────
add_route() {
    local method="$1" path="$2" desc="$3"
    local row="        {\"${method}\", \"${path}\", \"${desc}\"},"
    if grep -qF "\"${method}\", \"${path}\"" "$ENDPOINTS_HPP"; then return; fi
    awk -v row="$row" '
        /^    };$/ && !done && seen { print row; done=1 }
        /inline const std::vector<EndpointInfo>& get_endpoints/ { seen=1 }
        { print }
    ' "$ENDPOINTS_HPP" >"$ENDPOINTS_HPP.tmp" && mv "$ENDPOINTS_HPP.tmp" "$ENDPOINTS_HPP"
    grep -qF "\"${method}\", \"${path}\"" "$ENDPOINTS_HPP" ||
        die "failed to insert route ${method} ${path} into Endpoints.hpp — add by hand"
}
add_route "POST" "/api/v1/orgs" "Create an organization (admin); creator becomes owner"
add_route "GET" "/api/v1/orgs" "List organizations (admin)"
add_route "GET" "/api/v1/orgs/mine" "List the caller's organizations with their role"
add_route "POST" "/api/v1/orgs/{id}/switch" "Mint an access token scoped to an organization"
add_route "GET" "/api/v1/orgs/{id}/members" "List organization members (admin or owner)"
add_route "POST" "/api/v1/orgs/{id}/members" "Add an organization member (admin or owner)"
add_route "PATCH" "/api/v1/orgs/{id}/members/{user_id}" "Change a member's tenant role (admin or owner)"
add_route "DELETE" "/api/v1/orgs/{id}/members/{user_id}" "Remove an organization member (admin or owner)"
echo "==> Added 8 routes to $ENDPOINTS_HPP"

# ── 14. OpenAPI block ────────────────────────────────────────────────────
if ! grep -q "^  /api/v1/orgs:" "$OPENAPI"; then
    cat >>"$OPENAPI" <<'EOF'

  /api/v1/orgs:
    get:
      summary: List organizations (admin; offset-paginated)
      tags: [orgs]
      security: [{ BearerAuth: [] }]
      parameters:
        - { name: limit,  in: query, schema: { type: integer, minimum: 1, maximum: 200, default: 50 } }
        - { name: offset, in: query, schema: { type: integer, minimum: 0, default: 0 } }
      responses:
        '200': { description: "{ data, total, limit, offset }" }
        '403': { description: Not an admin }
    post:
      summary: Create an organization (admin); the creator is bound as its owner
      tags: [orgs]
      security: [{ BearerAuth: [] }]
      requestBody:
        required: true
        content:
          application/json:
            schema:
              type: object
              required: [name]
              properties: { name: { type: string, minLength: 1, maxLength: 255 } }
      responses:
        '201': { description: Created }
        '400': { description: Validation failed }
        '403': { description: Not an admin }
  /api/v1/orgs/mine:
    get:
      summary: List the caller's organizations, each annotated with their tenant role
      tags: [orgs]
      security: [{ BearerAuth: [] }]
      responses:
        '200': { description: "{ data: [ { ...organization, role } ] }" }
        '401': { description: Not authenticated }
  /api/v1/orgs/{id}/switch:
    parameters:
      - { name: id, in: path, required: true, schema: { type: string, format: uuid } }
    post:
      summary: Mint an access token scoped to this organization (member only; resets on refresh)
      tags: [orgs]
      security: [{ BearerAuth: [] }]
      responses:
        '200': { description: "{ access } (+ access cookie in cookie mode; refresh cookie untouched)" }
        '401': { description: Not authenticated }
        '403': { description: Not a member of this organization }
  /api/v1/orgs/{id}/members:
    parameters:
      - { name: id, in: path, required: true, schema: { type: string, format: uuid } }
    get:
      summary: List members with email (admin, or owner of this organization)
      tags: [orgs]
      security: [{ BearerAuth: [] }]
      responses:
        '200': { description: "{ data: [ { user_id, email, role, created_at } ] }" }
        '403': { description: Not an admin nor this org's owner }
    post:
      summary: Add a member by user_id or email (admin, or owner of this organization)
      tags: [orgs]
      security: [{ BearerAuth: [] }]
      requestBody:
        required: true
        content:
          application/json:
            schema:
              type: object
              required: [role]
              properties:
                user_id: { type: string, format: uuid }
                email: { type: string, format: email }
                role: { type: string, enum: [owner, member, viewer] }
      responses:
        '201': { description: Created }
        '400': { description: Validation failed }
        '403': { description: Not an admin nor this org's owner }
        '404': { description: No user with this email }
        '409': { description: Already a member }
  /api/v1/orgs/{id}/members/{user_id}:
    parameters:
      - { name: id, in: path, required: true, schema: { type: string, format: uuid } }
      - { name: user_id, in: path, required: true, schema: { type: string, format: uuid } }
    patch:
      summary: Change a member's tenant role (admin, or owner of this organization)
      tags: [orgs]
      security: [{ BearerAuth: [] }]
      requestBody:
        required: true
        content:
          application/json:
            schema:
              type: object
              required: [role]
              properties: { role: { type: string, enum: [owner, member, viewer] } }
      responses:
        '200': { description: Updated }
        '403': { description: Not an admin nor this org's owner }
        '404': { description: No such membership }
        '409': { description: last_owner — cannot demote the last owner }
    delete:
      summary: Remove a member (admin, or owner of this organization)
      tags: [orgs]
      security: [{ BearerAuth: [] }]
      responses:
        '200': { description: Removed }
        '403': { description: Not an admin nor this org's owner }
        '404': { description: No such membership }
        '409': { description: last_owner — cannot remove the last owner }
EOF
    echo "==> Appended OpenAPI block to $OPENAPI"
fi

# ── 14b. docs/module-deps.txt — declare the new tenancy edges ────────────
# The module-DAG gate (scripts/check-module-deps.sh, added after the first
# version of this kit) fails any cross-directory #include whose edge is not
# declared. The kit introduces api -> tenancy plus tenancy's own
# dependencies; insert each at its sorted position. Skipped edge-by-edge if
# already present, and entirely on a fork that predates the gate.
DEPS="$ROOT/docs/module-deps.txt"
if [[ -f "$DEPS" ]]; then
    add_edge_before() {
        local edge="$1" anchor="$2"
        grep -qxF "$edge" "$DEPS" && return 0
        printf '%s\n' "$edge" >"$TMP/edge.frag"
        insert_before "$DEPS" "$anchor" "$TMP/edge.frag"
    }
    add_edge_before 'api -> tenancy' 'api -> utils'
    add_edge_before 'tenancy -> database' 'webhooks -> jobs'
    add_edge_before 'tenancy -> repositories' 'webhooks -> jobs'
    add_edge_before 'tenancy -> security' 'webhooks -> jobs'
    echo "==> Declared tenancy edges in $DEPS"
fi

# ── 15. Tests ────────────────────────────────────────────────────────────
cat >"$ROOT/tests/unit/test_org_permissions.cpp" <<'EOF'
/**
 * @file test_org_permissions.cpp
 * @brief The tenant-role permission matrix as code. Pure unit test: no DB,
 *        no Drogon. The headline test is not "owner can do everything" but
 *        DENY-BY-DEFAULT: an unknown role, unknown resource and unknown
 *        action must all give false — a denylist that fails open is the bug
 *        this table exists to prevent. Installed by scripts/add-orgs.sh.
 *
 * When you add a resource row to kMatrix, add it to kAllResources below —
 * the static_assert breaks the build the moment the two lists diverge.
 */

#include <cstddef>
#include <iterator>
#include <string>

#include <gtest/gtest.h>

#include "tenancy/OrgPermissions.hpp"

namespace {

using namespace Tenancy::OrgPerm;

/// Every resource in the matrix — so the sweeps below walk the whole table,
/// not a hand-copied subset.
constexpr const char* kAllResources[] = {
    Resource::kWidgets,
    Resource::kMembers,
};

constexpr const char* kAllActions[] = {Action::kRead, Action::kWrite};

/// The list above is written BY HAND while the comment promises a full-table
/// sweep. A resource added to kMatrix but forgotten here would silently go
/// untested — this breaks the build at the moment the lists diverge.
static_assert(std::size(Tenancy::OrgPerm::detail::kMatrix) == std::size(kAllResources),
              "resource added to kMatrix — add it to kAllResources or the sweeps won't see it");

TEST(OrgPermissions, OwnerHasFullAccessEverywhere) {
    for (const auto* res : kAllResources) {
        EXPECT_TRUE(allows("owner", res, Action::kRead)) << res;
        EXPECT_TRUE(allows("owner", res, Action::kWrite)) << res;
    }
}

TEST(OrgPermissions, MemberWritesWidgetsButNeverMembers) {
    EXPECT_TRUE(allows("member", Resource::kWidgets, Action::kRead));
    EXPECT_TRUE(allows("member", Resource::kWidgets, Action::kWrite));
    // "" = invisible: no write AND no read.
    EXPECT_FALSE(allows("member", Resource::kMembers, Action::kRead));
    EXPECT_FALSE(allows("member", Resource::kMembers, Action::kWrite));
}

TEST(OrgPermissions, ViewerIsReadOnlyAtBest) {
    EXPECT_TRUE(allows("viewer", Resource::kWidgets, Action::kRead));
    for (const auto* res : kAllResources) {
        EXPECT_FALSE(allows("viewer", res, Action::kWrite)) << res;
    }
    EXPECT_FALSE(allows("viewer", Resource::kMembers, Action::kRead));
}

TEST(OrgPermissions, UnknownRoleDeniedEverywhere) {
    for (const auto* res : kAllResources) {
        for (const auto* act : kAllActions) {
            EXPECT_FALSE(allows("superuser", res, act)) << res << " / " << act;
            EXPECT_FALSE(allows("", res, act)) << res << " / " << act;
        }
    }
}

TEST(OrgPermissions, UnknownResourceDeniedForEveryRole) {
    for (const auto* role : Tenancy::OrgPerm::detail::kRoles) {
        for (const auto* act : kAllActions) {
            EXPECT_FALSE(allows(role, "no_such_resource", act)) << role << " / " << act;
        }
    }
}

TEST(OrgPermissions, UnknownActionDenied) {
    EXPECT_FALSE(allows("owner", Resource::kWidgets, "delete"));
    EXPECT_FALSE(allows("owner", Resource::kWidgets, ""));
}

TEST(OrgPermissions, UnfilledCellDenies) {
    // grant_for is templated over the row array precisely so this test can
    // prove the third deny-by-default dimension: a cell left nullptr (role
    // added to kRoles, row not updated) closes access instead of opening it.
    constexpr Tenancy::OrgPerm::detail::MatrixRow rows[] = {
        {"gadgets", {"rw", nullptr, ""}},
    };
    EXPECT_STREQ(Tenancy::OrgPerm::detail::grant_for(rows, "owner", "gadgets"), "rw");
    EXPECT_EQ(Tenancy::OrgPerm::detail::grant_for(rows, "member", "gadgets"), nullptr);
}

}  // namespace
EOF
echo "==> Created tests/unit/test_org_permissions.cpp"

cat >"$ROOT/tests/integration/test_org_context.cpp" <<'EOF'
/**
 * @file test_org_context.cpp
 * @brief Integration tests for Tenancy::org_context_of(): the fail-closed
 *        resolver behind the API_REQUIRE_ORG guard. Installed by
 *        scripts/add-orgs.sh.
 *
 * Mints real HS256 access tokens (Security::Auth::issue_hs256_jwt) and runs
 * them through the same Authenticator::verify_jwt() path the auth middleware
 * uses in production — this exercises the `org` claim parsing in
 * src/security/Auth.hpp, not just OrgContext's own logic. The resulting
 * principal is stamped onto a bare request (TestHelpers::authed, mirroring
 * what the middleware does after verification) before calling
 * org_context_of(req) directly.
 *
 * Coverage:
 *   - a member of the claimed org gets a context carrying their role
 *   - a claimed org the caller has no membership in yields nullopt
 *   - a token with no `org` claim at all yields nullopt
 */

#include <optional>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "tenancy/OrgContext.hpp"
#include "tenancy/OrgMemberRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;

namespace {

constexpr const char* kSecret = "test-jwt-secret-for-org-context-padding";

class OrgContextTest : public TestHelpers::CoreBackedTest {
protected:
    std::string config_file_name() const override { return "org_context_test_config.json"; }

    void config_overrides(json& cfg) override {
        cfg["auth"]["mode"] = "jwt";
        cfg["auth"]["jwt"]["secret"] = kSecret;
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
    }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        // TRUNCATE users CASCADE clears org_members through its FK; the
        // organizations rows themselves reference no one, so a plain DELETE
        // finishes the wipe.
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE users CASCADE");
            txn.exec("DELETE FROM organizations");
            return 0;
        });
    }

    /// Confirmed "User"-role user — these tests don't need a principal built
    /// ahead of time, just the id to reference in claims/memberships.
    std::string seed_user(const std::string& email) {
        Repositories::RoleRepository roles;
        Repositories::UserRepository users;
        auto role = roles.find_by_name("User");
        if (!role) {
            ADD_FAILURE() << "role 'User' missing — seed migration?";
            throw std::runtime_error("seed role missing: User");
        }
        auto created = users.create(
            email, std::string("$argon2id$placeholder"), std::nullopt, std::nullopt, role->id, /*confirmed=*/true);
        return created.id;
    }

    Tenancy::Organization seed_org(const std::string& name) {
        Tenancy::OrganizationRepository orgs;
        return orgs.create(name);
    }

    /// Mint an HS256 access token with the given claims and run it through
    /// Authenticator::verify_jwt() exactly like the auth middleware would —
    /// this is what actually exercises the `p.org = claims.value("org", "")`
    /// line in Auth.hpp, rather than hand-constructing an AuthPrincipal.
    static std::optional<Security::Auth::AuthPrincipal> mint_principal(const json& claims) {
        const std::string token = Security::Auth::issue_hs256_jwt(claims, kSecret);
        std::string err;
        return Security::Auth::get().verify_jwt(token, err);
    }
};

TEST_F(OrgContextTest, MemberGetsContextWithRole) {
    auto user_id = seed_user("member@example.com");
    auto org = seed_org("Member Org");
    Tenancy::OrgMemberRepository members;
    members.add(org.id, user_id, "member");

    auto principal = mint_principal({{"sub", user_id}, {"org", org.id}});
    ASSERT_TRUE(principal.has_value());
    EXPECT_EQ(principal->org, org.id);

    auto req = TestHelpers::authed(*principal);
    auto ctx = Tenancy::org_context_of(req);
    ASSERT_TRUE(ctx.has_value());
    EXPECT_EQ(ctx->org_id, org.id);
    EXPECT_EQ(ctx->role, "member");
    EXPECT_EQ(ctx->user_id, user_id);
}

TEST_F(OrgContextTest, NonMemberGetsNoContext) {
    auto user_id = seed_user("outsider@example.com");
    // The claimed org exists, but this user has no membership row in it —
    // e.g. a stale claim after removal, or a forged/foreign org id.
    auto foreign_org = seed_org("Foreign Org");

    auto principal = mint_principal({{"sub", user_id}, {"org", foreign_org.id}});
    ASSERT_TRUE(principal.has_value());
    EXPECT_EQ(principal->org, foreign_org.id);

    auto req = TestHelpers::authed(*principal);
    EXPECT_FALSE(Tenancy::org_context_of(req).has_value());
}

TEST_F(OrgContextTest, MissingOrgClaimGetsNoContext) {
    auto user_id = seed_user("noorg@example.com");
    // Even with a real membership on file, a token minted without an `org`
    // claim (0/>1 memberships at login, or a pre-multi-tenancy token) must
    // fail closed rather than silently guessing which org to scope to.
    auto org = seed_org("Unclaimed Org");
    Tenancy::OrgMemberRepository members;
    members.add(org.id, user_id, "owner");

    auto principal = mint_principal({{"sub", user_id}});
    ASSERT_TRUE(principal.has_value());
    EXPECT_TRUE(principal->org.empty());

    auto req = TestHelpers::authed(*principal);
    EXPECT_FALSE(Tenancy::org_context_of(req).has_value());
}

}  // namespace
EOF
echo "==> Created tests/integration/test_org_context.cpp"

cat >"$ROOT/tests/integration/test_org_scoped_crud.cpp" <<'EOF'
/**
 * @file test_org_scoped_crud.cpp
 * @brief Integration test for Tenancy::OrgCrudBase — proves that
 *        find_in_org/list_in_org/count_in_org isolate rows by organization.
 *        The test table is created/dropped in the fixture itself (no
 *        migration exists for it, and none should — it's test-only
 *        scaffolding). Installed by scripts/add-orgs.sh.
 */

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "tenancy/OrgScoped.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

namespace {

struct TestWidget {
    std::string id, org_id, name;
    template <typename Row>
    static TestWidget from_row(const Row& r) {
        return {r["id"].template as<std::string>(),
                r["org_id"].template as<std::string>(),
                r["name"].template as<std::string>()};
    }
};

class TestWidgetRepository : public Tenancy::OrgCrudBase<TestWidgetRepository, TestWidget, std::string> {
public:
    static constexpr const char* kTable = "test_org_widgets";
    static constexpr const char* kColumns = "id, org_id, name";
    static constexpr const char* kIdColumn = "id";
    static constexpr const char* kOrderBy = "id";
    static constexpr const char* kOrgColumn = "org_id";
};

class OrgScopedTest : public TestHelpers::CoreBackedTest {
protected:
    std::string config_file_name() const override { return "org_scoped_test_config.json"; }

    void config_overrides(nlohmann::json& cfg) override {
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
    }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Database::get().execute_write([](auto& txn) {
            txn.exec(
                "CREATE TABLE IF NOT EXISTS test_org_widgets ("
                "id UUID PRIMARY KEY DEFAULT gen_random_uuid(), "
                "org_id UUID NOT NULL, name TEXT NOT NULL)");
            return 0;
        });
    }

    void TearDown() override {
        if (Database::is_initialized()) {
            Database::get().execute_write([](auto& txn) {
                txn.exec("DROP TABLE IF EXISTS test_org_widgets");
                return 0;
            });
        }
        TestHelpers::CoreBackedTest::TearDown();
    }
};

TEST_F(OrgScopedTest, RowsAreIsolatedByOrg) {
    Tenancy::OrganizationRepository orgs;
    auto a = orgs.create("Org A");
    auto b = orgs.create("Org B");
    Database::get().execute_write([&](auto& txn) {
        txn.exec_params("INSERT INTO test_org_widgets (org_id, name) VALUES ($1,'wa'),($2,'wb')", a.id, b.id);
        return 0;
    });

    TestWidgetRepository repo;
    auto in_a = repo.list_in_org(a.id);
    ASSERT_EQ(in_a.size(), 1u);
    EXPECT_EQ(in_a[0].name, "wa");

    EXPECT_EQ(repo.count_in_org(b.id), 1);

    auto wb_id = repo.list_in_org(b.id)[0].id;
    EXPECT_FALSE(repo.find_in_org(wb_id, a.id));  // a's org must not see b's row
}

}  // namespace
EOF
echo "==> Created tests/integration/test_org_scoped_crud.cpp"

cat >"$ROOT/tests/api/test_organizations_api.cpp" <<'EOF'
/**
 * @file test_organizations_api.cpp
 * @brief Controller tests for OrganizationsController — tenant CRUD,
 *        membership management, and the org-switch access-token mint.
 *        Installed by scripts/add-orgs.sh.
 *
 * Direct-controller-invocation idiom (tests/api bucket): stamp a hand-built
 * AuthPrincipal onto a bare request via TestHelpers::authed, call the
 * handler, assert on the response. The test harness runs auth.mode=jwt
 * (TestHelpers::minimal_config), so every guard in the controller is live.
 * SwitchIssuesOrgToken additionally decodes the minted token with the real
 * Security::Auth::verify_hs256_jwt() to prove the `org` claim round-trips.
 */

#include <optional>
#include <string>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/OrganizationsController.hpp"
#include "domain/Role.hpp"
#include "domain/User.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "tenancy/OrgMemberRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_fixtures.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

namespace {

constexpr const char* kSecret = "test-jwt-secret-for-organizations-api-pad";

class OrganizationsApiTest : public TestHelpers::CoreBackedTest {
protected:
    Api::OrganizationsController orgs_ctrl;

    std::string config_file_name() const override { return "organizations_api_test_config.json"; }

    void config_overrides(json& cfg) override {
        cfg["auth"]["mode"] = "jwt";
        cfg["auth"]["jwt"]["secret"] = kSecret;
        // Cookie mode on: switchOrg must set the access cookie, so the suite
        // runs under the same cookie config production does.
        cfg["auth"]["cookies"]["enabled"] = true;
        cfg["auth"]["cookies"]["secure"] = false;  // no https in the test container
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
    }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        // TRUNCATE users CASCADE clears org_members through its FK; the
        // organizations rows themselves reference no one — DELETE finishes it.
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE users CASCADE");
            txn.exec("DELETE FROM organizations");
            return 0;
        });
    }

    /// Create a user with the named app role and return both the row and a
    /// pre-built principal. `org` is left empty; org-scoped tests set it via
    /// with_org() below once a membership row exists.
    struct Pair {
        Domain::User user;
        Security::Auth::AuthPrincipal principal;
    };

    Pair seed_user(const std::string& email, const std::string& role_name = "User") {
        Repositories::RoleRepository roles;
        Repositories::UserRepository users;
        auto role = roles.find_by_name(role_name);
        if (!role) {
            ADD_FAILURE() << "role " << role_name << " missing — seed migration?";
            throw std::runtime_error("seed role missing: " + role_name);
        }
        auto created = users.create(
            email, std::string("$argon2id$placeholder"), std::nullopt, std::nullopt, role->id, /*confirmed=*/true);
        auto fresh = users.find(created.id);
        Pair p;
        p.user = fresh ? *fresh : created;
        p.principal.subject = p.user.id;
        if (p.user.role)
            p.principal.roles.push_back(p.user.role->name);
        // Mirrors what AuthController::mint_session would put in claims.
        p.principal.raw_claims = json{{"sub", p.user.id}, {"permissions", p.user.role ? p.user.role->permissions : 0u}};
        return p;
    }

    /// Stamp the `org` claim onto an already-built principal — the piece
    /// org_context_of() reads to resolve the caller's membership.
    static Security::Auth::AuthPrincipal with_org(Security::Auth::AuthPrincipal p, const std::string& org_id) {
        p.org = org_id;
        return p;
    }

    Tenancy::Organization seed_org(const std::string& name) {
        Tenancy::OrganizationRepository orgs;
        return orgs.create(name);
    }

    static HttpRequestPtr authed(const Security::Auth::AuthPrincipal& p, HttpMethod method = Get) {
        return TestHelpers::authed(p, method);
    }

    static HttpRequestPtr authed_json(const Security::Auth::AuthPrincipal& p,
                                      const json& body,
                                      HttpMethod method = Post) {
        return TestHelpers::authed_json(p, body, method);
    }
};

// ── Organization CRUD (admin-gated) ─────────────────────────────────────

TEST_F(OrganizationsApiTest, AdminCreatesOrganization) {
    auto admin = seed_user("admin@example.com", "Administrator");
    auto req = authed_json(admin.principal, {{"name", "Acme LLC"}});
    HttpResponsePtr resp;
    orgs_ctrl.createOrganization(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k201Created);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["data"]["name"].get<std::string>(), "Acme LLC");
    EXPECT_EQ(body["data"]["status"].get<std::string>(), "active");

    Tenancy::OrganizationRepository repo;
    auto found = repo.find(body["data"]["id"].get<std::string>());
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->name, "Acme LLC");
}

// The org creator must land in org_members as "owner", or they're stuck
// outside the org they just created — unable to reach any of the owner-gated
// member-management routes for it. Checks both surfaces a frontend would use
// to notice: GET /orgs/mine (role annotation) and GET /orgs/{id}/members
// (roster, via the admin-fallback path of require_admin_or_org_owner).
TEST_F(OrganizationsApiTest, AdminCreatingOrgBecomesOwner) {
    auto admin = seed_user("owner-admin@example.com", "Administrator");
    auto req = authed_json(admin.principal, {{"name", "Newly Owned LLC"}});
    HttpResponsePtr create_resp;
    orgs_ctrl.createOrganization(req, [&](const HttpResponsePtr& r) { create_resp = r; });
    ASSERT_NE(create_resp, nullptr);
    ASSERT_EQ(create_resp->statusCode(), k201Created);
    auto create_body = json::parse(std::string(create_resp->body()));
    const std::string org_id = create_body["data"]["id"].get<std::string>();

    Tenancy::OrgMemberRepository members;
    auto membership = members.find_membership(org_id, admin.user.id);
    ASSERT_TRUE(membership.has_value());
    EXPECT_EQ(membership->role, "owner");

    HttpResponsePtr mine_resp;
    orgs_ctrl.mine(authed(admin.principal), [&](const HttpResponsePtr& r) { mine_resp = r; });
    ASSERT_NE(mine_resp, nullptr);
    ASSERT_EQ(mine_resp->statusCode(), k200OK);
    auto mine_body = json::parse(std::string(mine_resp->body()));
    ASSERT_TRUE(mine_body["data"].is_array());
    bool found_in_mine = false;
    for (const auto& item : mine_body["data"]) {
        if (item["id"].get<std::string>() == org_id) {
            EXPECT_EQ(item["role"].get<std::string>(), "owner");
            found_in_mine = true;
        }
    }
    EXPECT_TRUE(found_in_mine);

    HttpResponsePtr roster_resp;
    orgs_ctrl.listMembers(
        authed(admin.principal), [&](const HttpResponsePtr& r) { roster_resp = r; }, org_id);
    ASSERT_NE(roster_resp, nullptr);
    ASSERT_EQ(roster_resp->statusCode(), k200OK);
    auto roster_body = json::parse(std::string(roster_resp->body()));
    ASSERT_TRUE(roster_body["data"].is_array());
    ASSERT_EQ(roster_body["data"].size(), 1U);
    EXPECT_EQ(roster_body["data"][0]["user_id"].get<std::string>(), admin.user.id);
    EXPECT_EQ(roster_body["data"][0]["role"].get<std::string>(), "owner");
}

TEST_F(OrganizationsApiTest, NonAdminCannotCreate) {
    auto user = seed_user("user@example.com", "User");
    auto req = authed_json(user.principal, {{"name", "Nope LLC"}});
    HttpResponsePtr resp;
    orgs_ctrl.createOrganization(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

// ── /orgs/mine ──────────────────────────────────────────────────────────

TEST_F(OrganizationsApiTest, MineListsMembershipsWithRole) {
    auto user = seed_user("member@example.com", "User");
    auto org = seed_org("Member Org LLC");
    Tenancy::OrgMemberRepository members;
    members.add(org.id, user.user.id, "member");

    HttpResponsePtr resp;
    orgs_ctrl.mine(authed(user.principal), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    ASSERT_TRUE(body["data"].is_array());
    ASSERT_EQ(body["data"].size(), 1U);
    EXPECT_EQ(body["data"][0]["id"].get<std::string>(), org.id);
    EXPECT_EQ(body["data"][0]["role"].get<std::string>(), "member");
}

// ── /orgs/{id}/switch ───────────────────────────────────────────────────

TEST_F(OrganizationsApiTest, SwitchIssuesOrgToken) {
    auto user = seed_user("switcher@example.com", "User");
    auto org = seed_org("Switch Org LLC");
    Tenancy::OrgMemberRepository members;
    members.add(org.id, user.user.id, "viewer");

    auto req = authed(user.principal, Post);
    HttpResponsePtr resp;
    orgs_ctrl.switchOrg(
        req, [&](const HttpResponsePtr& r) { resp = r; }, org.id);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    ASSERT_TRUE(body.contains("access"));

    std::string err;
    auto claims = Security::Auth::verify_hs256_jwt(body["access"].get<std::string>(), kSecret, err);
    ASSERT_TRUE(claims.has_value()) << "decode failed: " << err;
    EXPECT_EQ((*claims).value("org", ""), org.id);
    EXPECT_EQ((*claims).value("sub", ""), user.user.id);
    EXPECT_EQ((*claims).value("typ", ""), "access");

    // Cookie-mode clients read the access token from the HttpOnly access
    // cookie, not the JSON body — switchOrg must set it or a browser session
    // keeps sending the OLD access cookie and the switch silently doesn't
    // take effect. Exactly one cookie: the refresh cookie must stay
    // untouched (empty refresh_token passed to set_session_cookies).
    const auto& cookies = resp->cookies();
    ASSERT_EQ(cookies.size(), 1u);
    const auto& access_cookie_name = Security::Auth::get().config().cookies.access_name;
    auto cookie_it = cookies.find(access_cookie_name);
    ASSERT_NE(cookie_it, cookies.end()) << "missing cookie " << access_cookie_name;
    EXPECT_EQ(cookie_it->second.value(), body["access"].get<std::string>());
    EXPECT_TRUE(cookie_it->second.isHttpOnly());
}

TEST_F(OrganizationsApiTest, SwitchForbiddenForNonMember) {
    auto user = seed_user("outsider@example.com", "User");
    auto org = seed_org("Outsider Org LLC");

    auto req = authed(user.principal, Post);
    HttpResponsePtr resp;
    orgs_ctrl.switchOrg(
        req, [&](const HttpResponsePtr& r) { resp = r; }, org.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

// ── Member management (admin or owner) ──────────────────────────────────

TEST_F(OrganizationsApiTest, OwnerManagesMembers) {
    auto org = seed_org("Owner-Managed Org LLC");
    auto owner = seed_user("owner@example.com", "User");
    auto target = seed_user("target@example.com", "User");
    Tenancy::OrgMemberRepository members;
    members.add(org.id, owner.user.id, "owner");
    auto owner_principal = with_org(owner.principal, org.id);

    // Add.
    auto add_req = authed_json(owner_principal, {{"user_id", target.user.id}, {"role", "viewer"}}, Post);
    HttpResponsePtr add_resp;
    orgs_ctrl.addMember(
        add_req, [&](const HttpResponsePtr& r) { add_resp = r; }, org.id);
    ASSERT_NE(add_resp, nullptr);
    EXPECT_EQ(add_resp->statusCode(), k201Created);

    // Change role.
    auto patch_req = authed_json(owner_principal, {{"role", "member"}}, Patch);
    HttpResponsePtr patch_resp;
    orgs_ctrl.updateMemberRole(
        patch_req, [&](const HttpResponsePtr& r) { patch_resp = r; }, org.id, target.user.id);
    ASSERT_NE(patch_resp, nullptr);
    EXPECT_EQ(patch_resp->statusCode(), k200OK);
    auto patch_body = json::parse(std::string(patch_resp->body()));
    EXPECT_EQ(patch_body["data"]["role"].get<std::string>(), "member");

    // Remove.
    auto del_req = authed(owner_principal, Delete);
    HttpResponsePtr del_resp;
    orgs_ctrl.removeMember(
        del_req, [&](const HttpResponsePtr& r) { del_resp = r; }, org.id, target.user.id);
    ASSERT_NE(del_resp, nullptr);
    EXPECT_EQ(del_resp->statusCode(), k200OK);
    EXPECT_FALSE(members.find_membership(org.id, target.user.id).has_value());
}

TEST_F(OrganizationsApiTest, ViewerCannotManageMembers) {
    auto org = seed_org("Viewer Org LLC");
    auto viewer = seed_user("viewer@example.com", "User");
    auto target = seed_user("target2@example.com", "User");
    Tenancy::OrgMemberRepository members;
    members.add(org.id, viewer.user.id, "viewer");
    auto viewer_principal = with_org(viewer.principal, org.id);

    auto req = authed_json(viewer_principal, {{"user_id", target.user.id}, {"role", "viewer"}}, Post);
    HttpResponsePtr resp;
    orgs_ctrl.addMember(
        req, [&](const HttpResponsePtr& r) { resp = r; }, org.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

TEST_F(OrganizationsApiTest, LastOwnerNotRemovable) {
    auto org = seed_org("Sole Owner Org LLC");
    auto owner = seed_user("soleowner@example.com", "User");
    Tenancy::OrgMemberRepository members;
    members.add(org.id, owner.user.id, "owner");
    auto owner_principal = with_org(owner.principal, org.id);

    // Cannot remove the last owner.
    auto del_req = authed(owner_principal, Delete);
    HttpResponsePtr del_resp;
    orgs_ctrl.removeMember(
        del_req, [&](const HttpResponsePtr& r) { del_resp = r; }, org.id, owner.user.id);
    ASSERT_NE(del_resp, nullptr);
    EXPECT_EQ(del_resp->statusCode(), k409Conflict);
    auto del_body = json::parse(std::string(del_resp->body()));
    EXPECT_EQ(del_body["error"].get<std::string>(), "last_owner");
    EXPECT_TRUE(members.find_membership(org.id, owner.user.id).has_value());

    // Cannot demote the last owner either.
    auto patch_req = authed_json(owner_principal, {{"role", "viewer"}}, Patch);
    HttpResponsePtr patch_resp;
    orgs_ctrl.updateMemberRole(
        patch_req, [&](const HttpResponsePtr& r) { patch_resp = r; }, org.id, owner.user.id);
    ASSERT_NE(patch_resp, nullptr);
    EXPECT_EQ(patch_resp->statusCode(), k409Conflict);
    auto patch_body = json::parse(std::string(patch_resp->body()));
    EXPECT_EQ(patch_body["error"].get<std::string>(), "last_owner");
    auto still = members.find_membership(org.id, owner.user.id);
    ASSERT_TRUE(still.has_value());
    EXPECT_EQ(still->role, "owner");
}

}  // namespace
EOF
echo "==> Created tests/api/test_organizations_api.cpp"

# ── Done ─────────────────────────────────────────────────────────────────
cat <<EOF

Multi-tenancy starter installed. Next steps:

  1. Permission matrix: src/tenancy/OrgPermissions.hpp ships with an EXAMPLE
     'widgets' resource. Replace it with your real resources (one row per
     org-scoped resource) and mirror each row in kAllResources in
     tests/unit/test_org_permissions.cpp. Deny-by-default: a resource with
     no row 403s for everyone.
  2. Org-scoped resources: ./scripts/new-resource.sh <Entity> --org-scoped
     (repositories inherit Tenancy::OrgCrudBase — no unscoped reads exist).
  3. Frontend: 'make frontend-gen-api' regenerates the typed client with the
     new /orgs routes; /auth/me now carries org_role for menu gating.
  4. Switch semantics (document in your SPA): the org claim is minted
     automatically only for users with EXACTLY ONE membership. Multi-org
     users must POST /orgs/{id}/switch — and repeat it after every
     /auth/refresh, which resets the claim to the single-membership default.
  5. Org guards are NOT a no-op under AUTH_MODE=none (unlike admin guards):
     org-scoped endpoints require real auth. Keep auth.mode=jwt.
  6. Verify: ./scripts/check-openapi-drift.sh && ./scripts/check-routes-registered.sh
             && ./scripts/check-test-buckets.sh && make test

See docs/ORGS.md for the concepts (two role layers, fail-closed context,
instant revocation via the per-request membership read).
EOF
