#!/usr/bin/env bash
# Directory-level include-dependency gate for src/.
#
# The include graph IS the architecture: the phase-1 modularity work
# measured that Guards.hpp -> Core.hpp made every controller transitively
# include Kafka/PayPal/OTel, so touching Core recompiled 22 of 64 TUs.
# This gate keeps the untangled graph untangled: every cross-directory
# `#include "dir/File.hpp"` in src/ must correspond to an edge declared in
# docs/module-deps.txt, plus three hard-coded rules that stay forbidden
# even if someone adds the edge to the declaration file:
#
#   1. utils/ includes nothing outside utils/ (it is the bottom layer).
#   2. core/Core.hpp (the composition root that pulls every subsystem) is
#      included only by the binary entry points and api/HealthController.hpp
#      — everyone else uses the tiny core/Modules.hpp.
#   3. webhooks -> email is dead (it closed the email/jobs/webhooks cycle).
#
# Pure bash+grep, runs in well under a second, no build needed.
set -euo pipefail

REPO="${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
SRC="$REPO/src"
DEPS="$REPO/docs/module-deps.txt"

[ -d "$SRC" ] || {
    echo "check-module-deps: $SRC not found" >&2
    exit 1
}
[ -f "$DEPS" ] || {
    echo "check-module-deps: $DEPS not found — the gate has no edge declaration to check against" >&2
    exit 1
}

# Files (relative to src/) that may include the composition root. Everything
# else must use core/Modules.hpp for the module switches / shutdown flag.
# core/Core.cpp is Core.hpp's own body file (de-inlined into app_core).
CORE_HPP_ALLOWED="main.cpp worker_main.cpp api/HealthController.hpp core/Core.cpp"

fail=0

# ── the declaration itself must not legalize a hard-forbidden edge ───────────
allowed_edges="$(sed 's/#.*//' "$DEPS" | grep -E '^[a-z_]+ -> [a-z_]+$' || true)"
if [ -z "$allowed_edges" ]; then
    echo "check-module-deps: no edges parsed from $DEPS — a gate with an empty" >&2
    echo "  allowlist would reject everything (or a typo'd one would check nothing)." >&2
    exit 1
fi
while IFS= read -r edge; do
    from="${edge%% -> *}"
    to="${edge##* -> }"
    if [ "$from" = "webhooks" ] && [ "$to" = "email" ]; then
        echo "ERROR: docs/module-deps.txt declares the FORBIDDEN edge 'webhooks -> email'" >&2
        echo "  — that edge closed the email/jobs/webhooks include cycle. The shared curl" >&2
        echo "  bootstrap lives in utils/CurlInit.hpp; use that instead of legalizing it." >&2
        fail=1
    fi
    if [ "$from" = "utils" ] && [ "$to" != "utils" ]; then
        echo "ERROR: docs/module-deps.txt declares 'utils -> $to' — utils/ is the bottom" >&2
        echo "  layer and may not depend on any other src/ directory." >&2
        fail=1
    fi
done <<<"$allowed_edges"

# ── actual edges: every project include in src/, attributed to its file ──────
# Output of the pipeline: "<file>|<from_dir> -> <to_dir>|<included_path>".
edges_seen=""
while IFS='|' read -r file edge inc; do
    from="${edge%% -> *}"
    to="${edge##* -> }"

    # Rule 1: utils/ is the bottom layer.
    if [ "$from" = "utils" ] && [ "$to" != "utils" ]; then
        echo "ERROR: forbidden edge utils -> $to" >&2
        echo "  $file includes \"$inc\" — utils/ may not depend on other src/ directories." >&2
        fail=1
        continue
    fi

    # Rule 3: the webhooks -> email cycle edge stays dead.
    if [ "$from" = "webhooks" ] && [ "$to" = "email" ]; then
        echo "ERROR: forbidden edge webhooks -> email" >&2
        echo "  $file includes \"$inc\" — this edge closed the email/jobs/webhooks include" >&2
        echo "  cycle. For curl_global_init use utils/CurlInit.hpp." >&2
        fail=1
        continue
    fi

    # Rule 2: core/Core.hpp only from the explicit allowlist.
    if [ "$inc" = "core/Core.hpp" ]; then
        ok=0
        for allowed in $CORE_HPP_ALLOWED; do
            [ "$file" = "src/$allowed" ] && ok=1
        done
        if [ "$ok" -eq 0 ]; then
            echo "ERROR: forbidden include of the composition root: $file includes \"core/Core.hpp\"" >&2
            echo "  Only these may include Core.hpp: $(echo "$CORE_HPP_ALLOWED" | sed 's/[^ ]*/src\/&/g')." >&2
            echo "  For Core::<module>_enabled() / Core::is_shutting_down() include \"core/Modules.hpp\"." >&2
            fail=1
            continue
        fi
    fi

    # Same-directory includes are always fine.
    [ "$from" = "$to" ] && continue

    if ! printf '%s\n' "$allowed_edges" | grep -qxF "$from -> $to"; then
        echo "ERROR: undeclared edge $from -> $to" >&2
        echo "  $file includes \"$inc\", but 'docs/module-deps.txt' has no '$from -> $to' line." >&2
        echo "  Either drop the include or (if the dependency is genuinely intended)" >&2
        echo "  declare the edge there in the same PR." >&2
        fail=1
        continue
    fi
    edges_seen="$edges_seen$from -> $to"$'\n'
done < <(
    grep -rn --include='*.hpp' --include='*.cpp' '#include "' "$SRC" |
        sed -E 's|^'"$SRC"'/([^:]+):[0-9]+:[[:space:]]*#include "([^"]+)".*|\1\|\2|' |
        awk -F'|' '{
            from = $1; sub(/\/[^\/]*$/, "", from)
            if (from == $1) from = "root"          # top-level file (main.cpp)
            to = $2; sub(/\/[^\/]*$/, "", to)
            if (to == $2) to = "root"              # bare include (version.hpp)
            printf "src/%s|%s -> %s|%s\n", $1, from, to, $2
        }' | LC_ALL=C sort -u
)

# ── stale declarations: warn, don't fail ─────────────────────────────────────
# An edge nobody uses anymore is latent permission for a regression; trim it.
while IFS= read -r edge; do
    [ -n "$edge" ] || continue
    if ! printf '%s' "$edges_seen" | grep -qxF "$edge"; then
        echo "WARNING: docs/module-deps.txt declares '$edge' but no include uses it — consider removing the line." >&2
    fi
done <<<"$allowed_edges"

if [ "$fail" -ne 0 ]; then
    exit 1
fi

edge_count="$(printf '%s' "$edges_seen" | sort -u | grep -c . || true)"
echo "✓ module deps OK: $edge_count cross-directory edge(s), all declared in docs/module-deps.txt"
