#!/usr/bin/env bash
#
# One-shot REMOVER for the content module (posts / uploads / sitemap) — the
# inverse of what PR #11 added. The content module is the template's worked
# example of a full feature module; a fork that doesn't publish articles can
# drop it entirely. Invoked by `init-project.sh --minimal`, or run standalone
# at any time BEFORE you build on the module.
#
# What it does, kept in lock-step with the triple-sync CI gates:
#   1. Deletes the module's own files: controllers (+ .cpp bodies), domain
#      struct, repository, src/storage/, migration 006_add_posts.sql, the
#      module's unit/integration tests, the admin SPA pages.
#   2. Strips every `init-project:content:start` … `init-project:content:end`
#      marker block from the shared files (Guards/Modules/Strings, e2e +
#      config tests, helm charts, nginx configs, docs/CONFIG.md, SPA
#      manifest/queryKeys/Dashboard, docker envs).
#   3. Pattern-edits the structured files markers can't carry (JSON has no
#      comments): config.json storage/content blocks + public_paths CSV,
#      the helm ConfigMap's rendered config.json, openapi.yaml path blocks,
#      Endpoints.hpp rows, Api.hpp includes, Core.cpp storage wiring,
#      docs/module-deps.txt edges.
#   4. Regenerates frontend/src/lib/api/schema.gen.ts from the shrunk
#      openapi.yaml (CI diffs the committed copy against a fresh render).
#   5. Verifies no functional reference survived.
#
# After this script the tree passes check-openapi-drift /
# check-routes-registered / check-module-deps /
# check-config-sync by construction. Migration numbering keeps a gap at 006 —
# the runner applies files in numeric order and does not require contiguity.
#
# NOT idempotent by design: it refuses to run twice (the files it edits may
# have moved on). See REMOVING-THE-DEMO.md for what is and isn't "demo".
#
# Usage:
#   ./scripts/remove-content-module.sh
set -euo pipefail

die() {
    echo "ERROR: $*" >&2
    exit 1
}

if [[ $# -gt 0 ]]; then
    echo "Usage: $0    (no arguments — one-shot remover; see REMOVING-THE-DEMO.md)" >&2
    exit 2
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

MARK_START='init-project:content:start'
MARK_END='init-project:content:end'

[[ -f src/api/PostsController.hpp ]] ||
    die "src/api/PostsController.hpp not found — the content module is already removed (or this isn't a template checkout)"

# GNU sed accepts `-i`, BSD/macOS sed requires `-i ''`.
if sed --version >/dev/null 2>&1; then
    SED_INPLACE=(sed -i)
else
    SED_INPLACE=(sed -i '')
fi

# ── 1. Whole files that ARE the module ───────────────────────────────────
MODULE_FILES=(
    src/api/PostsController.hpp
    src/api/PostsController.cpp
    src/api/UploadController.hpp
    src/api/UploadController.cpp
    src/api/ContentPagesController.hpp
    src/api/ContentPagesController.cpp
    src/domain/Post.hpp
    src/repositories/PostRepository.hpp
    src/storage/Storage.hpp
    src/storage/Storage.cpp
    migrations/006_add_posts.sql
    tests/integration/test_posts_api.cpp
    tests/integration/test_post_repository.cpp
    tests/integration/test_content_pages.cpp
    tests/integration/test_uploads_api.cpp
    tests/unit/test_storage.cpp
    tests/unit/test_storage_list.cpp
    tests/unit/test_upload_validation.cpp
    frontend/src/pages/admin/Posts.tsx
    frontend/src/pages/admin/Media.tsx
)
for f in "${MODULE_FILES[@]}"; do
    [[ -e "$f" ]] || die "$f not found — tree diverged from the template; remove the module by hand"
    rm -f "$f"
done
rmdir src/storage 2>/dev/null || true
echo "==> Deleted ${#MODULE_FILES[@]} module files (+ src/storage/)"

# ── 2. Marker blocks in shared files ─────────────────────────────────────
# Every file listed MUST contain at least one balanced marker pair — a
# missing marker means the block moved and this script rotted; fail loudly.
strip_marked() {
    local f="$1"
    grep -q "$MARK_START" "$f" || die "no '$MARK_START' marker in $f — script out of date, patch by hand"
    awk -v s="$MARK_START" -v e="$MARK_END" '
        index($0, s) { depth++; next }
        index($0, e) { depth--; if (depth < 0) exit 2; just_removed = 1; next }
        depth == 0 {
            # A block flanked by blank lines would leave TWO of them behind —
            # swallow one so clang-format (MaxEmptyLinesToKeep) stays happy.
            if (just_removed && last_blank && $0 ~ /^[[:space:]]*$/) {
                just_removed = 0
                next
            }
            just_removed = 0
            print
            last_blank = ($0 ~ /^[[:space:]]*$/)
        }
        END { if (depth != 0) exit 2 }
    ' "$f" >"$f.tmp" || die "unbalanced content markers in $f"
    mv "$f.tmp" "$f"
}
MARKED_FILES=(
    src/core/Modules.hpp
    src/api/Guards.hpp
    src/api/Middleware.cpp
    src/utils/Strings.hpp
    tests/e2e/test_http_e2e.cpp
    tests/unit/test_config.cpp
    frontend/nginx.conf
    frontend/src/routes/manifest.tsx
    frontend/src/lib/api/queryKeys.ts
    frontend/src/pages/admin/Dashboard.tsx
    helm/cpp-frontend/templates/configmap.yaml
    helm/mi-fitness-api/values.yaml
    helm/mi-fitness-api/templates/deployment.yaml
    helm/mi-fitness-api/templates/secret.yaml
    helm/cpp-env/values-demo.yaml
    docs/CONFIG.md
    docker/.env.everything
)
for f in "${MARKED_FILES[@]}"; do
    strip_marked "$f"
done
echo "==> Stripped marker blocks from ${#MARKED_FILES[@]} shared files"

# ── 3a. Api.hpp includes / Core.cpp storage wiring / misc single lines ───
"${SED_INPLACE[@]}" \
    -e '\|#include "api/ContentPagesController.hpp"|d' \
    -e '\|#include "api/PostsController.hpp"|d' \
    -e '\|#include "api/UploadController.hpp"|d' \
    src/api/Api.hpp
"${SED_INPLACE[@]}" \
    -e '\|#include "storage/Storage.hpp"|d' \
    -e '\|Storage::initialize(cfg);|d' \
    src/core/Core.cpp
# Dashboard: the two lucide icons only the removed tiles used.
"${SED_INPLACE[@]}" -e '/^  FileText,$/d' -e '/^  Image,$/d' \
    frontend/src/pages/admin/Dashboard.tsx
# Compose env line.
"${SED_INPLACE[@]}" -e '/CONTENT_ENABLED:/d' docker/docker-compose.yml
# Module-dependency DAG: the storage node and the api->storage edge.
"${SED_INPLACE[@]}" \
    -e '/^api -> storage$/d' \
    -e '/^core -> storage$/d' \
    -e '/^storage -> utils$/d' \
    docs/module-deps.txt
# Helm Secret: drop the s3 term from the render-this-Secret-at-all condition
# (the s3-secret-key data block itself was a marker block above).
"${SED_INPLACE[@]}" -e 's| \.Values\.storage\.s3\.secretKey||' \
    helm/mi-fitness-api/templates/secret.yaml
echo "==> Patched includes, storage wiring, compose env, module-deps, helm secret"

# ── 3b. Endpoints.hpp — the module's rows in Api::get_endpoints() ────────
ENDPOINTS_RE='"/api/v1/posts|"/api/v1/public/posts|"/posts/\{slug\}"|"/sitemap\.xml"|"/api/v1/admin/uploads|"/uploads/\{key\}"'
grep -vE "$ENDPOINTS_RE" src/api/Endpoints.hpp >src/api/Endpoints.hpp.tmp
mv src/api/Endpoints.hpp.tmp src/api/Endpoints.hpp
echo "==> Removed content routes from src/api/Endpoints.hpp"

# ── 3c. openapi.yaml — whole path blocks ─────────────────────────────────
# A block starts at a 2-space-indented path key and runs until the next
# 2-space key (or a column-0 key). Keys removed = exactly the module's ten.
awk '
    {
        if ($0 ~ /^[^ ]/) skip = 0
        else if ($0 ~ /^  [^ ]/) {
            if ($0 ~ /^  \/(api\/v1\/posts(\/\{id\}(\/preview-token)?)?|api\/v1\/public\/posts(\/\{slug\})?|posts\/\{slug\}|sitemap\.xml|uploads\/\{key\}|api\/v1\/admin\/uploads(\/\{name\})?):[ ]*$/)
                skip = 1
            else
                skip = 0
        }
        if (!skip) print
    }
' docs/openapi.yaml >docs/openapi.yaml.tmp
mv docs/openapi.yaml.tmp docs/openapi.yaml
echo "==> Removed content path blocks from docs/openapi.yaml"

# ── 3d. config.json / config.sample.json — storage+content blocks ────────
# JSON carries no comments, so no markers: delete a top-level block by brace
# counting from its 2-space-indented key. ${VAR:-default} placeholders keep
# braces balanced per line, so per-line counting is safe here.
delete_json_block() {
    local f="$1" key="$2"
    grep -q "^  \"$key\": {" "$f" || die "no \"$key\" block in $f — patch by hand"
    awk -v re="^  \"$key\": \\{" '
        skip == 0 && $0 ~ re { skip = 1; depth = 0 }
        skip == 1 {
            depth += split($0, _o, "{") - split($0, _c, "}")
            if (depth == 0) skip = 0
            next
        }
        { print }
    ' "$f" >"$f.tmp"
    mv "$f.tmp" "$f"
}
for f in config/config.json config/config.sample.json; do
    delete_json_block "$f" storage
    delete_json_block "$f" content
done
# config.json's api.public_paths FULL-override default carries the module's
# public paths — drop exactly that segment (the sample's copy predates them).
"${SED_INPLACE[@]}" \
    -e 's|/posts/\*,/sitemap\.xml,/api/v1/public/posts,/api/v1/public/posts/\*,/uploads/\*,||' \
    config/config.json config/config.sample.json
# …and the API_PUBLIC_PATHS row in docs/CONFIG.md spells the same default out.
# shellcheck disable=SC2016  # the backticks are Markdown literals, not expansions
"${SED_INPLACE[@]}" \
    -e 's|, `/api/v1/public/posts`, `/api/v1/public/posts/\*`, `/posts/\*`, `/sitemap\.xml`, `/uploads/\*`||' \
    docs/CONFIG.md
# Helm ConfigMap renders config.json too: its "storage" block sits between
# "mail" and "messaging" and contains template comments (brace counting is
# unsafe over {{- /* */}}), so cut [storage-start, messaging-start).
awk '
    /^      "storage": \{/ { skip = 1 }
    /^      "messaging": \{/ { skip = 0 }
    !skip { print }
' helm/mi-fitness-api/templates/configmap.yaml >helm/mi-fitness-api/templates/configmap.yaml.tmp
mv helm/mi-fitness-api/templates/configmap.yaml.tmp helm/mi-fitness-api/templates/configmap.yaml
echo "==> Removed storage/content config blocks (config.json, sample, helm ConfigMap)"

# ── 4. Regenerate the typed frontend client ──────────────────────────────
# CI's frontend job regenerates schema.gen.ts from docs/openapi.yaml and
# fails if the committed copy is stale — so regenerate it now, with the
# pinned devDependency (npm ci) rather than whatever npx would fetch.
if command -v npm >/dev/null 2>&1; then
    if [[ ! -x frontend/node_modules/.bin/openapi-typescript ]]; then
        echo "==> Installing frontend deps for schema regeneration (one-time npm ci)…"
        (cd frontend && npm ci --no-audit --no-fund >/dev/null) ||
            die "npm ci failed — run 'make frontend-install && make frontend-gen-api' by hand"
    fi
    (cd frontend && npm run gen:api >/dev/null)
    echo "==> Regenerated frontend/src/lib/api/schema.gen.ts"
else
    echo "WARNING: npm not found — regenerate the typed client before pushing:" >&2
    echo "         make frontend-install && make frontend-gen-api" >&2
fi

# ── 5. Verify nothing functional survived ────────────────────────────────
LEFTOVER_RE='PostsController|UploadController|ContentPagesController|PostRepository|#include "domain/Post\.hpp"|#include "storage/|require_content_enabled|content_enabled|CONTENT_ENABLED|content\.enabled|STORAGE_BACKEND|STORAGE_LOCAL_ROOT|STORAGE_PUBLIC_BASE_URL|S3_SECRET_KEY|/api/v1/public/posts|/api/v1/admin/uploads|sitemap\.xml|init-project:content'
leftovers="$(grep -rInE "$LEFTOVER_RE" \
    src tests config helm docker frontend/src frontend/nginx.conf \
    docs/openapi.yaml docs/CONFIG.md docs/module-deps.txt Makefile 2>/dev/null |
    grep -v 'node_modules' || true)"
if [[ -n "$leftovers" ]]; then
    echo "" >&2
    echo "==> INCOMPLETE: content-module references survived the removal:" >&2
    printf '%s\n' "$leftovers" | sed 's/^/  /' >&2
    exit 1
fi
echo "==> Verified: no functional content-module references remain."

cat <<'EOF'

Content module removed. Notes:
  * migrations keep a numbering gap at 006 — harmless, the runner sorts.
  * narrative docs (docs/EXAMPLES.md, docs/CONVENTIONS.md, ADRs, archived
    plans under docs/superpowers/) still DESCRIBE the module as a worked
    example; they are history, not wiring.
  * verify: ./scripts/check-openapi-drift.sh && ./scripts/check-routes-registered.sh
            && ./scripts/check-module-deps.sh
            && ./scripts/check-config-sync.sh && ./scripts/check-test-buckets.sh
EOF
