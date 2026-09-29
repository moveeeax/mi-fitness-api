#!/usr/bin/env bash
#
# Scaffold a full CRUD domain resource end to end, following docs/CONVENTIONS.md:
#   - src/domain/<Entity>.hpp            (struct + from_row + to_json)
#   - src/repositories/<Entity>Repository.hpp  (typed exceptions + find/list/count/create/remove)
#   - src/api/<Entity>sController.hpp     (list/create/get/delete, with_repo_errors)
#   - wires the #include into src/api/Api.hpp
#   - adds the routes to Api::get_endpoints() in src/api/Endpoints.hpp
#   - appends a path + schema block to docs/openapi.yaml
#   - tests/integration/test_<entity>.cpp     (Postgres/Redis-backed flow)
#   - tests/unit/test_<entity>_domain.cpp     (serialization; runs WITHOUT infra)
#
# Three shapes:
#   default       ADMIN-gated, global-scoped (admin sees/manages every row).
#   --owned       PER-USER: rows carry owner_id (FK to users), the controller
#             gates with API_REQUIRE_OWNER and scopes every repo call by the
#             caller via CrudBase find_owned/list_owned/count_owned — so it is
#             IDOR-safe by construction. Use this for anything a normal user owns.
#   --org-scoped  PER-TENANT: rows carry org_id (FK to organizations), the
#             repository inherits Tenancy::OrgCrudBase (find_in_org/list_in_org/
#             count_in_org — NO global find/list/count exist, a forgotten org
#             filter is a compile error), and the controller opens every handler
#             with the API_REQUIRE_ORG + API_REQUIRE_ORG_PERM pair. Requires the
#             orgs starter kit: run ./scripts/add-orgs.sh first.
#
# The generated code COMPILES as-is with a minimal {id, name, created_at}
# shape — edit the three files to add your real columns (keep struct /
# from_row / to_json / SQL in sync).
#
# Usage:
#   ./scripts/new-resource.sh <Entity> [--owned|--org-scoped]  # singular PascalCase
#   ./scripts/new-resource.sh Product                          # admin-gated, global
#   ./scripts/new-resource.sh Note --owned                     # per-user, owner-scoped
#   ./scripts/new-resource.sh Invoice --org-scoped             # per-tenant
#
set -euo pipefail

# --owned scaffolds a PER-USER resource: every row carries owner_id (FK to
# users), the controller gates with API_REQUIRE_OWNER and scopes every repo
# call by the caller, so one user can never touch another's rows.
# --org-scoped scaffolds a PER-TENANT resource on Tenancy::OrgCrudBase.
# Without either you get the default ADMIN-gated, global-scoped resource.
OWNED=0
ORG_SCOPED=0
ENTITY=""
for arg in "$@"; do
    case "$arg" in
    --owned) OWNED=1 ;;
    --org-scoped) ORG_SCOPED=1 ;;
    -*)
        echo "ERROR: unknown flag '$arg'" >&2
        exit 2
        ;;
    *)
        if [[ -n "$ENTITY" ]]; then
            echo "ERROR: unexpected extra argument '$arg'" >&2
            exit 2
        fi
        ENTITY="$arg"
        ;;
    esac
done

if [[ -z "$ENTITY" ]]; then
    echo "Usage: $0 <Entity> [--owned|--org-scoped]   (singular PascalCase, e.g. Product)" >&2
    exit 2
fi
if [[ ! "$ENTITY" =~ ^[A-Z][A-Za-z0-9]+$ ]]; then
    echo "ERROR: entity must be PascalCase (e.g. Product), got '$ENTITY'" >&2
    exit 2
fi
if [[ $OWNED -eq 1 && $ORG_SCOPED -eq 1 ]]; then
    echo "ERROR: --owned and --org-scoped are mutually exclusive (a row is scoped to a user OR to a tenant)" >&2
    exit 2
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [[ $ORG_SCOPED -eq 1 && ! -e "$ROOT/src/tenancy/OrgScoped.hpp" ]]; then
    echo "ERROR: --org-scoped requires the orgs starter kit (src/tenancy/ is missing)." >&2
    echo "       Run ./scripts/add-orgs.sh first — it installs Tenancy::OrgCrudBase, the" >&2
    echo "       org guards, the organizations/org_members tables and the org API." >&2
    exit 2
fi
# Derivations: lower-case singular, naive plural (append s), table = plural.
LOWER="$(printf '%s' "$ENTITY" | tr '[:upper:]' '[:lower:]')"
PLURAL="${LOWER}s"
# API version prefix — see docs/adr/0001-api-versioning.md. Bumping the default
# major is a one-line edit here; routes are emitted correct-by-construction.
API_VERSION="v1"
API_PREFIX="/api/${API_VERSION}"
ROUTE="${API_PREFIX}/${PLURAL}"
CONTROLLER="${ENTITY}sController"

DOMAIN_FILE="$ROOT/src/domain/${ENTITY}.hpp"
REPO_FILE="$ROOT/src/repositories/${ENTITY}Repository.hpp"
CTRL_FILE="$ROOT/src/api/${CONTROLLER}.hpp"
TEST_FILE="$ROOT/tests/integration/test_${LOWER}.cpp"
UNIT_TEST_FILE="$ROOT/tests/unit/test_${LOWER}_domain.cpp"
API_HPP="$ROOT/src/api/Api.hpp"
ENDPOINTS_HPP="$ROOT/src/api/Endpoints.hpp"
OPENAPI="$ROOT/docs/openapi.yaml"

for f in "$DOMAIN_FILE" "$REPO_FILE" "$CTRL_FILE"; do
    if [[ -e "$f" ]]; then
        echo "ERROR: $f already exists — refusing to overwrite" >&2
        exit 1
    fi
done

# ── Per-resource fragments, toggled by --owned / --org-scoped ────────────
# Literal $1/$2 inside these values are NOT re-expanded by the <<EOF heredocs
# (single expansion pass), so they pass through to the generated SQL verbatim.
if [[ $ORG_SCOPED -eq 1 ]]; then
    DOC_GATING="Org-scoped (per-tenant): every row belongs to an organization; handlers resolve the caller's OrgContext and scope every repo call by ctx.org_id"
    OWNER_FIELD="    std::string org_id;    // FK -> organizations.id (the tenant)
"
    OWNER_FROMROW="        e.org_id = row[\"org_id\"].template as<std::string>();
"
    OWNER_TOJSON="        {\"org_id\", e.org_id},
"
    REPO_COLUMNS="id, org_id, name, created_at"
    REPO_KOWNER="    static constexpr const char* kOrgColumn = \"org_id\";
"
    REPO_BASE="Tenancy::OrgCrudBase<${ENTITY}Repository, Domain::${ENTITY}, std::string>"
    REPO_BASE_INCLUDE="#include \"tenancy/OrgScoped.hpp\""
    REPO_CREATE_SIG="create(const std::string& name, const std::string& org_id)"
    REPO_CREATE_SQL="INSERT INTO ${PLURAL} (name, org_id) VALUES (\$1, \$2) RETURNING "
    REPO_CREATE_ARGS="name, org_id"
    REPO_REMOVE_SIG="remove(const std::string& id, const std::string& org_id)"
    REPO_REMOVE_SQL="DELETE FROM ${PLURAL} WHERE id = \$1 AND org_id = \$2 RETURNING id"
    REPO_REMOVE_ARGS="id, org_id"
    # The guard PAIR: bind the org context (403 fail-closed without a live
    # membership behind the org claim), then check the tenant-role matrix.
    # Deny-by-default: until you add a "${PLURAL}" row to
    # src/tenancy/OrgPermissions.hpp kMatrix, every request 403s.
    CTRL_GUARD_READ="API_REQUIRE_ORG(req, callback, ctx);
        API_REQUIRE_ORG_PERM(callback, ctx, \"${PLURAL}\", Tenancy::OrgPerm::Action::kRead);"
    CTRL_GUARD_WRITE="API_REQUIRE_ORG(req, callback, ctx);
        API_REQUIRE_ORG_PERM(callback, ctx, \"${PLURAL}\", Tenancy::OrgPerm::Action::kWrite);"
    CTRL_LIST="repo.list_in_org(ctx.org_id, page.limit, page.offset)"
    CTRL_COUNT="repo.count_in_org(ctx.org_id)"
    CTRL_FIND="repo.find_in_org(id, ctx.org_id)"
    CTRL_CREATE="repo.create(body[\"name\"].get<std::string>(), ctx.org_id)"
    CTRL_REMOVE="repo.remove(id, ctx.org_id)"
    SUMMARY_GATE="org-scoped"
    DESC_403="No org context, or tenant role not granted by the matrix"
    TEST_CASE="ListRequiresOrgContext"
    TEST_REQUEST="TestHelpers::make_request(Get)"
    TEST_ASSERT="    // Org-scoped: an unauthenticated request has no principal, hence no org
    // context — API_REQUIRE_ORG rejects it with 403 (fail-closed; the guard is
    // deliberately NOT a no-op under any auth mode). To exercise the 200 path,
    // seed an organization + membership and authenticate via TestHelpers with
    // a principal whose .org points at it (see tests/api/test_organizations_api.cpp).
    EXPECT_EQ(resp->statusCode(), k403Forbidden);"
    UNIT_OWNER_SET="    e.org_id = \"33333333-3333-3333-3333-333333333333\";
"
    UNIT_OWNER_ASSERT="    EXPECT_EQ(j[\"org_id\"], e.org_id);
"
elif [[ $OWNED -eq 1 ]]; then
    DOC_GATING="Owner-scoped (per-user): every row is scoped to the authenticated caller"
    OWNER_FIELD="    std::string owner_id;  // FK -> users.id (the authenticated caller)
"
    OWNER_FROMROW="        e.owner_id = row[\"owner_id\"].template as<std::string>();
"
    OWNER_TOJSON="        {\"owner_id\", e.owner_id},
"
    REPO_COLUMNS="id, owner_id, name, created_at"
    REPO_KOWNER="    static constexpr const char* kOwnerColumn = \"owner_id\";
"
    REPO_BASE="CrudBase<${ENTITY}Repository, Domain::${ENTITY}, std::string>"
    REPO_BASE_INCLUDE="#include \"repositories/CrudBase.hpp\""
    REPO_CREATE_SIG="create(const std::string& name, const std::string& owner_id)"
    REPO_CREATE_SQL="INSERT INTO ${PLURAL} (name, owner_id) VALUES (\$1, \$2) RETURNING "
    REPO_CREATE_ARGS="name, owner_id"
    REPO_REMOVE_SIG="remove(const std::string& id, const std::string& owner_id)"
    REPO_REMOVE_SQL="DELETE FROM ${PLURAL} WHERE id = \$1 AND owner_id = \$2 RETURNING id"
    REPO_REMOVE_ARGS="id, owner_id"
    CTRL_GUARD_READ="API_REQUIRE_OWNER(req, callback, owner);"
    CTRL_GUARD_WRITE="API_REQUIRE_OWNER(req, callback, owner);"
    CTRL_LIST="repo.list_owned(owner, page.limit, page.offset)"
    CTRL_COUNT="repo.count_owned(owner)"
    CTRL_FIND="repo.find_owned(id, owner)"
    CTRL_CREATE="repo.create(body[\"name\"].get<std::string>(), owner)"
    CTRL_REMOVE="repo.remove(id, owner)"
    SUMMARY_GATE="owner-scoped"
    DESC_403="Forbidden"
    TEST_CASE="ListRequiresOwner"
    TEST_REQUEST="TestHelpers::make_request(Get)"
    TEST_ASSERT="    // Owner-scoped: an unauthenticated request has no principal, so the guard
    // rejects it with 401 — the proof the per-user gate is wired (no IDOR via a
    // missing identity). Authenticate via TestHelpers to exercise the 200 path.
    EXPECT_EQ(resp->statusCode(), k401Unauthorized);"
    UNIT_OWNER_SET="    e.owner_id = \"22222222-2222-2222-2222-222222222222\";
"
    UNIT_OWNER_ASSERT="    EXPECT_EQ(j[\"owner_id\"], e.owner_id);
"
else
    DOC_GATING="Admin-gated"
    OWNER_FIELD=""
    OWNER_FROMROW=""
    OWNER_TOJSON=""
    REPO_COLUMNS="id, name, created_at"
    REPO_KOWNER=""
    REPO_BASE="CrudBase<${ENTITY}Repository, Domain::${ENTITY}, std::string>"
    REPO_BASE_INCLUDE="#include \"repositories/CrudBase.hpp\""
    REPO_CREATE_SIG="create(const std::string& name)"
    REPO_CREATE_SQL="INSERT INTO ${PLURAL} (name) VALUES (\$1) RETURNING "
    REPO_CREATE_ARGS="name"
    REPO_REMOVE_SIG="remove(const std::string& id)"
    REPO_REMOVE_SQL="DELETE FROM ${PLURAL} WHERE id = \$1 RETURNING id"
    REPO_REMOVE_ARGS="id"
    CTRL_GUARD_READ="API_REQUIRE_ADMIN(req, callback);"
    CTRL_GUARD_WRITE="API_REQUIRE_ADMIN(req, callback);"
    CTRL_LIST="repo.list(page.limit, page.offset)"
    CTRL_COUNT="repo.count()"
    CTRL_FIND="repo.find(id)"
    CTRL_CREATE="repo.create(body[\"name\"].get<std::string>())"
    CTRL_REMOVE="repo.remove(id)"
    SUMMARY_GATE="admin"
    DESC_403="Not an admin"
    TEST_CASE="ListReturnsEnvelope"
    TEST_REQUEST="TestHelpers::authed(TestFixtures::admin_principal())"
    TEST_ASSERT="    // CoreBackedTest runs auth.mode=jwt (see test_helpers.hpp), so the admin
    // guard is live — authenticate as admin to reach the handler.
    EXPECT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_TRUE(body.contains(\"data\"));
    EXPECT_TRUE(body.contains(\"total\"));"
    UNIT_OWNER_SET=""
    UNIT_OWNER_ASSERT=""
fi

# ── 1. Domain DTO ───────────────────────────────────────────────────────
cat >"$DOMAIN_FILE" <<EOF
/**
 * @file ${ENTITY}.hpp
 * @brief ${ENTITY} domain model. Generated by scripts/new-resource.sh.
 *
 * Add your real columns below — keep the struct, from_row() and to_json()
 * in sync (each field appears in all three). Never serialize secrets in
 * to_json (see tests/unit/test_domain_serialization.cpp).
 */

#pragma once

#include <string>

#include <pqxx/pqxx>

#include <nlohmann/json.hpp>

namespace Domain {

struct ${ENTITY} {
    std::string id;          // UUID v4 (text)
${OWNER_FIELD}    std::string name;        // TODO: replace with your real columns
    std::string created_at;

    template <typename Row>
    static ${ENTITY} from_row(const Row& row) {
        ${ENTITY} e;
        e.id = row["id"].template as<std::string>();
${OWNER_FROMROW}        e.name = row["name"].template as<std::string>();
        e.created_at = row["created_at"].template as<std::string>();
        return e;
    }
};

inline void to_json(nlohmann::json& j, const ${ENTITY}& e) {
    j = nlohmann::json{
        {"id", e.id},
${OWNER_TOJSON}        {"name", e.name},
        {"created_at", e.created_at},
    };
}

}  // namespace Domain
EOF
echo "==> Created $DOMAIN_FILE"

# ── 2. Repository ───────────────────────────────────────────────────────
cat >"$REPO_FILE" <<EOF
/**
 * @file ${ENTITY}Repository.hpp
 * @brief All SQL touching \`${PLURAL}\` lives here. Generated by
 *        scripts/new-resource.sh. Controllers never touch pqxx directly.
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <pqxx/pqxx>

#include "database/Database.hpp"
#include "domain/${ENTITY}.hpp"
${REPO_BASE_INCLUDE}
#include "repositories/RepoErrors.hpp"
#include "repositories/SqlErrors.hpp"

namespace Repositories {

// Derive from the generic repo bases so Api::with_repo_errors maps them to
// 409 / 404 instead of a bare 500 (see RepoErrors.hpp / HandlerSupport.hpp).
struct Duplicate${ENTITY} : ConflictError {
    Duplicate${ENTITY}() : ConflictError("${LOWER}_exists", "a ${LOWER} with that name already exists") {}
};

struct ${ENTITY}NotFound : NotFoundError {
    ${ENTITY}NotFound() : NotFoundError("${LOWER}") {}
};

class ${ENTITY}Repository : public ${REPO_BASE} {
public:
    // The CRTP base supplies the reads from these constants — only the
    // bespoke writes below are hand-written.
    static constexpr const char* kTable = "${PLURAL}";
    static constexpr const char* kColumns = "${REPO_COLUMNS}";
    static constexpr const char* kIdColumn = "id";
    static constexpr const char* kOrderBy = "created_at DESC";
${REPO_KOWNER}
    Domain::${ENTITY} ${REPO_CREATE_SIG} {
        return detail::translate_sql(
            [&] {
                return Database::get().execute_write([&](auto& txn) {
                    auto r = txn.exec_params(
                        std::string("${REPO_CREATE_SQL}") + kColumns, ${REPO_CREATE_ARGS});
                    return Domain::${ENTITY}::from_row(r[0]);
                });
            },
            detail::throw_on<Duplicate${ENTITY}>("23505"));
    }

    void ${REPO_REMOVE_SIG} {
        Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params("${REPO_REMOVE_SQL}", ${REPO_REMOVE_ARGS});
            if (r.empty())
                throw ${ENTITY}NotFound{};
            return 0;
        });
    }
};

}  // namespace Repositories
EOF
echo "==> Created $REPO_FILE"

# ── 3. Controller ───────────────────────────────────────────────────────
cat >"$CTRL_FILE" <<EOF
/**
 * @file ${CONTROLLER}.hpp
 * @brief ${ENTITY} CRUD endpoints. Generated by scripts/new-resource.sh.
 *        ${DOC_GATING}; follows docs/CONVENTIONS.md.
 */

#pragma once

#include <drogon/HttpController.h>
#include <drogon/drogon.h>

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/HandlerSupport.hpp"
#include "api/RequestUtils.hpp"
#include "api/Validation.hpp"
#include "domain/${ENTITY}.hpp"
#include "repositories/${ENTITY}Repository.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class ${CONTROLLER} : public HttpController<${CONTROLLER}> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(${CONTROLLER}::list${ENTITY}s, "${ROUTE}", Get);
    ADD_METHOD_TO(${CONTROLLER}::create${ENTITY}, "${ROUTE}", Post);
    ADD_METHOD_TO(${CONTROLLER}::get${ENTITY}, "${ROUTE}/{1}", Get);
    ADD_METHOD_TO(${CONTROLLER}::delete${ENTITY}, "${ROUTE}/{1}", Delete);
    METHOD_LIST_END

    void list${ENTITY}s(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        ${CTRL_GUARD_READ}
        const auto page = parse_page_params(req, /*default_limit=*/50, /*max_limit=*/200);
        with_repo_errors(callback, "list${ENTITY}s", [&] {
            Repositories::${ENTITY}Repository repo;
            auto items = ${CTRL_LIST};
            long total = ${CTRL_COUNT};
            callback(Response::paginated(to_json_array(items), total, page.limit, page.offset));
        });
    }

    void create${ENTITY}(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        ${CTRL_GUARD_WRITE}
        json body;
        if (!Validation::parse_body(req, body, callback)) return;
        Validation::Errors errs;
        Validation::require(errs, body, "name");
        Validation::string_length(errs, body, "name", 1, 255);
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }
        with_repo_errors(callback, "create${ENTITY}", [&] {
            Repositories::${ENTITY}Repository repo;
            auto created = ${CTRL_CREATE};
            callback(Response::created({{"data", json(created)}}));
        });
    }

    void get${ENTITY}(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback,
                      const std::string& id) {
        ${CTRL_GUARD_READ}
        if (!require_valid_uuid(id, callback))
            return;
        with_repo_errors(callback, "get${ENTITY}", [&] {
            Repositories::${ENTITY}Repository repo;
            auto found = ${CTRL_FIND};
            if (!found) {
                callback(ErrorResponse::not_found("${LOWER}"));
                return;
            }
            callback(Response::ok({{"data", json(*found)}}));
        });
    }

    void delete${ENTITY}(const HttpRequestPtr& req,
                         std::function<void(const HttpResponsePtr&)>&& callback,
                         const std::string& id) {
        ${CTRL_GUARD_WRITE}
        if (!require_valid_uuid(id, callback))
            return;
        with_repo_errors(callback, "delete${ENTITY}", [&] {
            Repositories::${ENTITY}Repository repo;
            ${CTRL_REMOVE};
            callback(Response::ok({{"message", "${ENTITY} deleted"}}));
        });
    }
};

}  // namespace Api
EOF
echo "==> Created $CTRL_FILE"

# ── 4. Wire #include into Api.hpp ───────────────────────────────────────
if ! grep -q "#include \"api/${CONTROLLER}.hpp\"" "$API_HPP"; then
    awk -v inc="#include \"api/${CONTROLLER}.hpp\"" '
        /#include "api\/JobsController.hpp"/ { print; print inc; next }
        { print }
    ' "$API_HPP" >"$API_HPP.tmp" && mv "$API_HPP.tmp" "$API_HPP"
    echo "==> Added #include to $API_HPP"
fi

# ── 5. Routes into get_endpoints() (Endpoints.hpp) ──────────────────────
add_route() {
    local method="$1" path="$2" desc="$3"
    local row="        {\"${method}\", \"${path}\", \"${desc}\"},"
    if grep -qF "\"${method}\", \"${path}\"" "$ENDPOINTS_HPP"; then return; fi
    awk -v row="$row" '
        /^    };$/ && !done && seen { print row; done=1 }
        /inline const std::vector<EndpointInfo>& get_endpoints/ { seen=1 }
        { print }
    ' "$ENDPOINTS_HPP" >"$ENDPOINTS_HPP.tmp" && mv "$ENDPOINTS_HPP.tmp" "$ENDPOINTS_HPP"
    if ! grep -qF "\"${method}\", \"${path}\"" "$ENDPOINTS_HPP"; then
        echo "ERROR: failed to insert route ${method} ${path} into Endpoints.hpp — add by hand" >&2
        exit 1
    fi
}
add_route "GET" "${ROUTE}" "List ${PLURAL}"
add_route "POST" "${ROUTE}" "Create ${LOWER}"
add_route "GET" "${ROUTE}/{id}" "Get ${LOWER}"
add_route "DELETE" "${ROUTE}/{id}" "Delete ${LOWER}"
echo "==> Added 4 routes to $ENDPOINTS_HPP"

# ── 6. OpenAPI block ────────────────────────────────────────────────────
if ! grep -q "^  ${ROUTE}:" "$OPENAPI"; then
    cat >>"$OPENAPI" <<EOF

  ${ROUTE}:
    get:
      summary: List ${PLURAL} (${SUMMARY_GATE}; offset-paginated)
      tags: [${PLURAL}]
      security: [{ BearerAuth: [] }]
      parameters:
        - { name: limit,  in: query, schema: { type: integer, minimum: 1, maximum: 200, default: 50 } }
        - { name: offset, in: query, schema: { type: integer, minimum: 0, default: 0 } }
      responses:
        '200': { description: "{ data, total, limit, offset }" }
        '403': { description: ${DESC_403} }
    post:
      summary: Create ${LOWER} (${SUMMARY_GATE})
      tags: [${PLURAL}]
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
        '403': { description: ${DESC_403} }
        '409': { description: Already exists }
  ${ROUTE}/{id}:
    parameters:
      - { name: id, in: path, required: true, schema: { type: string, format: uuid } }
    get:
      summary: Get ${LOWER} (${SUMMARY_GATE})
      tags: [${PLURAL}]
      security: [{ BearerAuth: [] }]
      responses:
        '200': { description: OK }
        '404': { description: Not found }
    delete:
      summary: Delete ${LOWER} (admin)
      tags: [${PLURAL}]
      security: [{ BearerAuth: [] }]
      responses:
        '200': { description: Deleted }
        '404': { description: Not found }
EOF
    echo "==> Appended OpenAPI block to $OPENAPI"
fi

# ── 7. Integration test skeleton ────────────────────────────────────────
if [[ ! -e "$TEST_FILE" ]]; then
    cat >"$TEST_FILE" <<EOF
/**
 * @file test_${LOWER}.cpp
 * @brief Integration tests for ${CONTROLLER}. Generated by new-resource.sh.
 *        Fill in once the migration + real columns exist.
 *
 * NB: this file lives in tests/integration/, so CMake's CONFIGURE_DEPENDS glob
 * compiles it automatically — there is no bucket list to register it in. It
 * needs a '${PLURAL}' table (the co-generated migration creates the stub).
 */

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/${CONTROLLER}.hpp"
#include "test_fixtures.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

namespace {

class ${ENTITY}sFlowTest : public TestHelpers::CoreBackedTest {
protected:
    Api::${CONTROLLER} controller;
    std::string config_file_name() const override { return "${LOWER}_flow_test_config.json"; }
    void config_overrides(json& cfg) override {
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
    }
};

TEST_F(${ENTITY}sFlowTest, ${TEST_CASE}) {
    HttpResponsePtr resp;
    controller.list${ENTITY}s(${TEST_REQUEST}, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
${TEST_ASSERT}
}

}  // namespace
EOF
    echo "==> Created $TEST_FILE"
fi

# ── 7b. Unit test (domain serialization — NO Postgres/Redis) ─────────────
# A fast test that runs in the UNIT bucket, so it executes even where the
# integration infra is absent (where CoreBackedTest skips). Catches to_json /
# field drift and is the place to assert no secret is serialized.
if [[ ! -e "$UNIT_TEST_FILE" ]]; then
    cat >"$UNIT_TEST_FILE" <<EOF
/**
 * @file test_${LOWER}_domain.cpp
 * @brief Unit tests for Domain::${ENTITY} (serialization). No infra — runs in
 *        the unit bucket. Generated by scripts/new-resource.sh.
 */

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "domain/${ENTITY}.hpp"

using json = nlohmann::json;

TEST(${ENTITY}Domain, ToJsonSerializesEveryField) {
    Domain::${ENTITY} e;
    e.id = "11111111-1111-1111-1111-111111111111";
${UNIT_OWNER_SET}    e.name = "example";
    e.created_at = "2026-01-01T00:00:00Z";

    json j = e;  // ADL to_json

    EXPECT_EQ(j["id"], e.id);
${UNIT_OWNER_ASSERT}    EXPECT_EQ(j["name"], e.name);
    EXPECT_EQ(j["created_at"], e.created_at);
    // NEVER serialize secrets in to_json. If you add a sensitive field, assert
    // it is ABSENT here (see tests/unit/test_domain_serialization.cpp).
}
EOF
    echo "==> Created $UNIT_TEST_FILE"
fi

# ── 8. Migration (CREATE TABLE + updated_at trigger) ────────────────────
# Co-generate the table the repo/DTO above reference, so the scaffold is not
# left pointing at a non-existent table. new-migration.sh --table emits the
# stub shape (id/name/created_at/updated_at) plus a trigger wired to the shared
# touch_updated_at() from migrations/000_updated_at_trigger.sql.
if grep -rqE "name[ '\"]*=*[ '\"]*${PLURAL}_table" "$ROOT/migrations" 2>/dev/null ||
    find "$ROOT/migrations" -maxdepth 1 -name "*_${PLURAL}*.sql" | grep -q .; then
    echo "==> Migration for '${PLURAL}' already exists — skipping"
else
    "$ROOT/scripts/new-migration.sh" "add_${PLURAL}_table" --table "${PLURAL}"
    if [[ $OWNED -eq 1 ]]; then
        # The repo/controller above scope by owner_id — make the schema match:
        # inject the FK column (after id) and an index for the per-user lookups.
        MIG_FILE="$(find "$ROOT/migrations" -maxdepth 1 -name "*_add_${PLURAL}_table.sql" | sort | tail -1)"
        if [[ -n "$MIG_FILE" ]]; then
            perl -i -pe 's/^(\s*id\s+UUID PRIMARY KEY DEFAULT gen_random_uuid\(\),)/$1\n    owner_id   UUID        NOT NULL REFERENCES users(id) ON DELETE CASCADE,/' "$MIG_FILE"
            printf '\nCREATE INDEX IF NOT EXISTS idx_%s_owner_id ON %s(owner_id);\n' "${PLURAL}" "${PLURAL}" >>"$MIG_FILE"
            echo "==> --owned: added owner_id FK + index to $(basename "$MIG_FILE")"
        fi
    elif [[ $ORG_SCOPED -eq 1 ]]; then
        # The repo/controller above scope by org_id — make the schema match:
        # inject the tenant FK column (after id) and the index every
        # list_in_org/count_in_org query will hit.
        MIG_FILE="$(find "$ROOT/migrations" -maxdepth 1 -name "*_add_${PLURAL}_table.sql" | sort | tail -1)"
        if [[ -n "$MIG_FILE" ]]; then
            perl -i -pe 's/^(\s*id\s+UUID PRIMARY KEY DEFAULT gen_random_uuid\(\),)/$1\n    org_id     UUID        NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,/' "$MIG_FILE"
            printf '\nCREATE INDEX IF NOT EXISTS idx_%s_org_id ON %s(org_id);\n' "${PLURAL}" "${PLURAL}" >>"$MIG_FILE"
            echo "==> --org-scoped: added org_id FK + index to $(basename "$MIG_FILE")"
        fi
    fi
fi

cat <<EOF

Done. Next steps (manual):
  1. Edit src/domain/${ENTITY}.hpp + ${ENTITY}Repository.hpp for your real
     columns, then mirror them in the generated migration (the table ships
     with the stub {id, name, created_at, updated_at} shape).
  2. Nothing to register: tests/integration/ is globbed by directory, so the
     new suite compiles as-is. ./scripts/check-test-buckets.sh just verifies it
     sits in the right bucket (no name clash with a unit suite).
  3. Frontend: types via 'make frontend-gen-api', page under src/pages/admin/.
  4. Verify:  ./scripts/check-openapi-drift.sh  &&  make test
EOF

if [[ $OWNED -eq 1 ]]; then
    cat <<EOF
  --owned note: the table has a FK owner_id -> users(id) ON DELETE CASCADE.
     Test fixtures that TRUNCATE users must use 'TRUNCATE users CASCADE' (or
     truncate ${PLURAL} too) — a plain TRUNCATE of a referenced table errors.
     Owner-scoped endpoints require AUTH_MODE != none (they need an identity).
EOF
fi

if [[ $ORG_SCOPED -eq 1 ]]; then
    cat <<EOF
  --org-scoped notes:
   * PERMISSION MATRIX (required, deny-by-default): the generated handlers gate
     with API_REQUIRE_ORG_PERM(..., "${PLURAL}", ...). Until you add a
     '${PLURAL}' row to kMatrix in src/tenancy/OrgPermissions.hpp (and mirror
     it in kAllResources in tests/unit/test_org_permissions.cpp), EVERY request
     — including reads — answers 403 org_role_denied.
   * The table has a FK org_id -> organizations(id) ON DELETE CASCADE; deleting
     an organization deletes its ${PLURAL}. Test fixtures that wipe
     organizations wipe these rows with them.
   * Org-scoped endpoints require AUTH_MODE != none AND an org-scoped access
     token (the 'org' claim + a live membership). Single-org users get the
     claim at login automatically; multi-org users POST /orgs/{id}/switch —
     and again after every /auth/refresh (docs/ORGS.md).
EOF
fi
