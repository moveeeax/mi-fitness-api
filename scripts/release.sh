#!/usr/bin/env bash
# release.sh <version> — bump every version point of a release in one command.
#
# Three releases in a row shipped with project(VERSION ...) still on 1.4.0,
# and values-stage.yaml carried image pins for GHCR tags that never existed:
# the release version lives in too many files to bump by hand. This script
# edits all of them, verifies the result with check-version-sync.sh, and
# leaves the commit to the human.
#
# What a release touches:
#   human-written CONTENT, script-driven mechanics:
#     CHANGELOG.md                     changelog.d/ fragments are folded into
#                                      '## [Unreleased]' (assemble-changelog.sh)
#                                      and the section is retitled to
#                                      '## [<version>] — YYYY-MM-DD' with a
#                                      fresh empty [Unreleased] kept on top; a
#                                      heading retitled by hand is accepted too
#   bumped by this script:
#     CMakeLists.txt                   project(VERSION ...) — the baked version
#                                      Core::version() / GET /health report
#     deploy/values-prod.yaml          1 image-tag pin (api)
#     deploy/values-worker-prod.yaml   1 image-tag pin (worker)
#     helm/{mi-fitness-api,mi-fitness-api-worker}/Chart.yaml
#                                      appVersion (default tag for standalone
#                                      installs + app.kubernetes.io/version)
#     .template-version                the stamp scripts/sync-upstream.sh reads
#                                      in a downstream fork as its patch base —
#                                      each release tarball must self-identify.
#                                      TEMPLATE REPO ONLY: in a fork (project.env
#                                      carries TEMPLATE_FORK=1) the stamp means
#                                      "last template release synced" and is
#                                      owned by sync-upstream.sh, so it is NOT
#                                      bumped by the fork's own releases.
#
# Deliberately NOT touched: vcpkg.json "version" (the Dockerfile keys the
# dependency-install layer on that manifest — bumping it rebuilds ~29 packages
# and blows CI timeouts) and the charts' packaging "version:" fields.
#
# The script commits nothing and tags nothing. It prints the diff and the
# next steps; the v<version> tag on master is what triggers release.yml.
#
# Usage: ./scripts/release.sh 1.6.0
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd -- "$SCRIPT_DIR/.." && pwd)"

die() {
    echo "release.sh: $*" >&2
    exit 1
}

command -v python3 >/dev/null 2>&1 || die "python3 required"

[ $# -eq 1 ] || die "usage: ./scripts/release.sh <version>   (e.g. 1.6.0)"
new_version="$1"

# --- validate: strict x.y.z semver ------------------------------------------
# Plain x.y.z only: release.yml strips the tag's leading v, GHCR carries the
# unprefixed form, and the helm pins are unprefixed on purpose (a v-prefixed
# pin pulls a tag that does not exist — cyber-accountant 80996a7).
case "$new_version" in
*[!0-9.]* | *..* | .* | *.) die "'$new_version' is not plain x.y.z semver (no v prefix, no pre-release)" ;;
esac
dots="${new_version//[0-9]/}"
[ "$dots" = ".." ] || die "'$new_version' is not plain x.y.z semver (need exactly three numeric fields)"

# --- validate: monotonic ------------------------------------------------------
current_version="$(sed -n 's/^project(.*VERSION[[:space:]]\{1,\}\([0-9][0-9.]*\).*/\1/p' \
    "$REPO/CMakeLists.txt" | head -1)"
[ -n "$current_version" ] || die "could not parse project(VERSION ...) from CMakeLists.txt"

python3 - "$current_version" "$new_version" <<'PY' || die "new version $new_version must be strictly greater than the current $current_version"
import sys
cur, new = (tuple(int(p) for p in a.split(".")) for a in sys.argv[1:3])
sys.exit(0 if new > cur else 1)
PY

# --- CHANGELOG: fold changelog.d/ fragments, then retitle [Unreleased] -------
# The content stays human-written — as changelog.d/<topic>.<type>.md fragments
# (parallel-safe; see changelog.d/README.md) or direct [Unreleased] edits.
# The mechanical moves are this script's job: fold the fragments in, retitle
# the heading, keep a fresh empty [Unreleased] on top. (The old flow died
# here with "go retitle by hand" — the one manual step left in a release. A
# heading already retitled by hand still passes.)
frag_count=0
[ -d "$REPO/changelog.d" ] && frag_count="$(find "$REPO/changelog.d" -maxdepth 1 \
    -type f ! -name 'README.md' | wc -l | tr -d ' ')"
if [ "$frag_count" -gt 0 ]; then
    "$SCRIPT_DIR/assemble-changelog.sh" | sed 's/^/  /'
fi

newest_released="$(sed -n 's/^## \[\([0-9][0-9.]*\)\].*/\1/p' "$REPO/CHANGELOG.md" | head -1)"
if [ "$newest_released" = "$new_version" ]; then
    if [ "$frag_count" -gt 0 ]; then
        die "CHANGELOG.md already carries a [$new_version] heading, but changelog.d/ fragments were just folded into [Unreleased] ABOVE it — move those bullets into [$new_version] (or pick a newer version) and re-run"
    fi
    echo "  CHANGELOG.md                 [$new_version] heading already in place"
else
    today="$(date +%Y-%m-%d)"
    python3 - "$REPO/CHANGELOG.md" "$new_version" "$today" <<'PY'
import re, sys
path, ver, today = sys.argv[1], sys.argv[2], sys.argv[3]
with open(path, encoding="utf-8") as fh:
    text = fh.read()
anchor = "## [Unreleased]"
n = text.count(anchor)
if n != 1:
    sys.exit("release.sh: CHANGELOG.md has %d '## [Unreleased]' heading(s) — expected exactly 1" % n)
m = re.search(r"(?ms)^## \[Unreleased\]\n(.*?)(?=^## |\Z)", text)
if not m.group(1).strip():
    sys.exit("release.sh: CHANGELOG.md's [Unreleased] section is empty — nothing to release.\n"
             "Write the release notes first: drop changelog.d/<topic>.<type>.md fragments\n"
             "(see changelog.d/README.md) or edit the [Unreleased] section directly.")
text = text.replace(anchor, anchor + "\n\n## [%s] — %s" % (ver, today), 1)
with open(path, "w", encoding="utf-8") as fh:
    fh.write(text)
PY
    echo "  retitled CHANGELOG.md        [Unreleased] -> [$new_version] — $today (fresh empty [Unreleased] on top)"
fi

# --- edit every version point (counted substitutions) ------------------------
# Each edit counts its own substitutions and fails loudly on the wrong count —
# a pattern that silently stops matching is exactly how version points drift.
bump() {
    # bump <file> <python-regex> <replacement> <expected-count>
    python3 - "$REPO/$1" "$2" "$3" "$4" <<'PY'
import re, sys
path, pattern, repl, expected = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
with open(path, encoding="utf-8") as fh:
    text = fh.read()
text, n = re.subn(pattern, repl, text)
if n != expected:
    sys.exit("release.sh: %s: replaced %d occurrence(s) of %r — expected exactly %d"
             % (path, n, pattern, expected))
with open(path, "w", encoding="utf-8") as fh:
    fh.write(text)
PY
}

bump CMakeLists.txt \
    '(?m)^(project\(.*VERSION[ \t]+)[0-9][0-9.]*' "\\g<1>$new_version" 1
echo "  bumped CMakeLists.txt        project(VERSION $new_version)"

for overlay in deploy/values-prod.yaml deploy/values-worker-prod.yaml; do
    bump "$overlay" \
        '(?m)^([ \t]*tag:[ \t]*")[0-9][0-9.]*(")' "\\g<1>$new_version\\g<2>" 1
    echo "  bumped $overlay  image-tag pin -> $new_version"
done

for chart in mi-fitness-api mi-fitness-api-worker; do
    bump "helm/$chart/Chart.yaml" \
        '(?m)^(appVersion:[ \t]*")[^"]*(")' "\\g<1>$new_version\\g<2>" 1
    echo "  bumped helm/$chart/Chart.yaml  appVersion -> $new_version"
done

if grep -qs '^TEMPLATE_FORK=1' "$REPO/project.env"; then
    echo "  skipped .template-version    (TEMPLATE_FORK=1 — owned by scripts/sync-upstream.sh)"
else
    bump .template-version \
        '(?m)^[0-9][0-9.]*$' "$new_version" 1
    echo "  bumped .template-version     -> $new_version"
fi

# --- verify ------------------------------------------------------------------
echo ""
"$SCRIPT_DIR/check-version-sync.sh"

# --- show the human what happened and what is next ---------------------------
echo ""
git -C "$REPO" --no-pager diff --stat -- \
    CHANGELOG.md changelog.d CMakeLists.txt .template-version deploy \
    helm/mi-fitness-api/Chart.yaml helm/mi-fitness-api-worker/Chart.yaml
cat <<EOF

Next steps (nothing has been committed):
  1. review:  git diff
  2. commit everything (changelog + bumps) as ONE commit:
       git commit -am 'chore(release): $new_version — <one-line summary>'
  3. after it lands on master, tag to trigger release.yml:
       git tag v$new_version && git push origin v$new_version
EOF
