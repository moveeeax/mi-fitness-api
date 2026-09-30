#!/usr/bin/env bash
#
# Proves the check-* gates bite.
#
# A gate that is only ever run against healthy trees is indistinguishable from
# `exit 0`, and this template's own history has both failure modes on record:
# check-test-buckets.sh read only its first directory argument (tests/api was
# never scanned), and a downstream fork shipped a gitleaks config that scanned
# with zero rules — green for months, checking nothing. So: for each of the nine
# gate scripts, copy the subtree it reads into a scratch dir, plant a known
# breakage, and require the gate to FAIL and to NAME what it found. Two planted
# breakages per gate (five for check-version-sync.sh, which also guards the
# helm image pins, Chart.yaml appVersions and the .template-version stamp; one
# for assemble-changelog.sh --check, the changelog-fragment format gate),
# twenty in all.
#
# Every case follows the same discipline (borrowed from the cyber-accountant
# fork's check-render-selftest.sh, which learned it the expensive way):
#
#   1. CONTROL — run the gate on the UNTOUCHED copy first and require a PASS.
#      Without the control, a case could "pass" because the gate rejects
#      everything, or because the copy was incomplete.
#   2. FINGERPRINT — the file list is fixed BEFORE the mutation; each file's
#      cksum+size is taken before and after. If the digests match, the mutator
#      changed nothing and the case would have re-tested the healthy tree.
#   3. COUNTED MUTATORS — every mutator is python3 and counts its own
#      substitutions, exiting nonzero when the count is wrong. A `sed` whose
#      pattern silently stopped matching is exactly how a selftest rots into a
#      green rubber stamp.
#   4. NEEDLES — the gate must exit nonzero AND its output must contain every
#      expected substring, so a gate that fails for the WRONG reason (crash,
#      missing file, unrelated assertion) does not count as a catch.
#
# The gates run against the scratch copies via the REPO_ROOT override each
# check script supports. The two check-helm-render.sh cases need helm + yq
# (same tools the helm-charts CI job installs); they are REQUIRED, not
# skipped — a skipped case reports success, which is the exact disease this
# script exists to cure.
#
# Usage: ./scripts/check-selftest.sh
set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd -- "$SCRIPT_DIR/.." && pwd)"

for tool in python3 git; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "check-selftest: $tool required" >&2
        exit 1
    }
done
# helm + yq are hard requirements, not a skip: the two check-helm-render.sh
# cases are the only proof that gate bites, and a silently skipped case is a
# green rubber stamp ("skipped job reports success").
for tool in helm yq; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "check-selftest: $tool required — the check-helm-render.sh cases cannot run" >&2
        echo "  without it, and skipping them would report success while testing nothing." >&2
        echo "  Install it (brew install $tool / see the helm-charts CI job) and re-run." >&2
        exit 1
    }
done

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
failures=0
cases=0

# --- plumbing ----------------------------------------------------------------

# copy_tree <dest> <path>... — copy the TRACKED files under the given repo
# paths into <dest>, preserving layout. git ls-files, not cp -R: untracked
# local litter (vendored charts/*.tgz, editor droppings) must not leak into
# the control run, or the control stops proving what CI would see.
copy_tree() {
    local dest="$1" f
    shift
    while IFS= read -r f; do
        mkdir -p "$dest/$(dirname "$f")"
        cp "$REPO/$f" "$dest/$f"
    done < <(git -C "$REPO" ls-files -- "$@")
}

# tree_files <dir> — every regular file under <dir>, one per line, sorted.
tree_files() {
    find "$1" -type f | LC_ALL=C sort
}

# digest_of — reads paths on stdin, emits "<path> <cksum> <bytes>" per path,
# or "<path> GONE" for one that no longer exists. The path list is captured
# BEFORE the mutation on purpose: only a change to a file the gate actually
# reads may count as a mutation, so litter a broken mutator leaves behind
# cannot masquerade as a successful one. POSIX tools only (cksum), because
# this runs on ubuntu CI runners and on the maintainer's macOS.
digest_of() {
    local file
    while IFS= read -r file; do
        if [[ -f "$file" ]]; then
            printf '%s %s\n' "$file" "$(cksum <"$file")"
        else
            printf '%s GONE\n' "$file"
        fi
    done
}

# run_case <label> <gate-script> <mutator> <copy-paths> <needle>...
#   label      — scratch-dir name and log tag
#   gate       — script under scripts/ to run with REPO_ROOT=<copy>; may carry
#                arguments ("assemble-changelog.sh --check"), word-split below
#   mutator    — shell function: takes the copy root, plants the breakage
#   copy-paths — space-separated repo paths the gate reads (word-split on
#                purpose; none of them contain spaces)
#   needles    — literal substrings the failing gate's output must contain
run_case() {
    local label="$1" gate="$2" mutator="$3" paths="$4"
    shift 4
    cases=$((cases + 1))
    local tree="$TMP/$label"
    mkdir -p "$tree"
    # shellcheck disable=SC2086 # deliberate word-split of the path list
    copy_tree "$tree" $paths

    local -a gate_cmd
    read -r -a gate_cmd <<<"$gate"
    gate_cmd[0]="$SCRIPT_DIR/${gate_cmd[0]}"

    local output status
    # Control: the untouched copy must PASS, or the case proves nothing.
    output="$(REPO_ROOT="$tree" "${gate_cmd[@]}" 2>&1)"
    status=$?
    if [[ "$status" -ne 0 ]]; then
        echo "SELFTEST FAIL [$label]: $gate fails on a HEALTHY tree (exit $status)," >&2
        echo "  so a failure on the broken one would prove nothing:" >&2
        printf '%s\n' "$output" | sed 's/^/    /' >&2
        failures=$((failures + 1))
        return
    fi

    # Now break it — and prove the breakage happened. A mutator whose anchor
    # rotted away would otherwise hand the gate the SAME tree it just passed,
    # and the case would report OK while testing nothing at all.
    local tracked digest_before digest_after
    tracked="$(tree_files "$tree")"
    digest_before="$(printf '%s\n' "$tracked" | digest_of)"
    if ! "$mutator" "$tree"; then
        echo "SELFTEST FAIL [$label]: the mutator $mutator itself failed — its anchor" >&2
        echo "  no longer matches the tree. Re-point the mutator at the shape the" >&2
        echo "  file has NOW; do not delete the case." >&2
        failures=$((failures + 1))
        return
    fi
    digest_after="$(printf '%s\n' "$tracked" | digest_of)"
    if [[ "$digest_before" == "$digest_after" ]]; then
        echo "SELFTEST FAIL [$label]: mutator changed NOTHING — every file under the" >&2
        echo "  copy is byte-identical, so this case would have re-tested the healthy" >&2
        echo "  tree and reported OK. A mutator that stops mutating is a gate that" >&2
        echo "  stops gating." >&2
        failures=$((failures + 1))
        return
    fi

    output="$(REPO_ROOT="$tree" "${gate_cmd[@]}" 2>&1)"
    status=$?
    if [[ "$status" -eq 0 ]]; then
        echo "SELFTEST FAIL [$label]: $gate PASSED the planted breakage:" >&2
        printf '%s\n' "$output" | sed 's/^/    /' >&2
        failures=$((failures + 1))
        return
    fi

    local missing=0 needle
    for needle in "$@"; do
        if ! printf '%s\n' "$output" | grep -qF -- "$needle"; then
            echo "SELFTEST FAIL [$label]: $gate failed, but never said: $needle" >&2
            missing=1
        fi
    done
    if [[ "$missing" -ne 0 ]]; then
        printf '%s\n' "$output" | sed 's/^/    /' >&2
        failures=$((failures + 1))
        return
    fi
    echo "SELFTEST OK   [$label]: $gate exit $status, named the planted breakage"
}

# --- the twenty breakages ----------------------------------------------------
# Each mutator takes the copy root as $1, edits in place, and COUNTS. An
# anchor that stops matching must fail the mutator loudly (see run_case), not
# no-op — every anchor below is a real line in this repo today.

# 1. A route registered in Api::get_endpoints() that the OpenAPI spec never
#    heard of — the "added the endpoint, forgot the spec" drift.
break_registry_ghost_route() {
    python3 - "$1/src/api/Endpoints.hpp" <<'PY'
import sys
path = sys.argv[1]
anchor = '{"GET", "/api/v1/jobs", "List jobs"},'
with open(path, encoding="utf-8") as fh:
    text = fh.read()
n = text.count(anchor)
if n != 1:
    sys.exit("break_registry_ghost_route: expected exactly 1 anchor row %r in %s, found %d"
             % (anchor, path, n))
text = text.replace(
    anchor,
    anchor + '\n        {"GET", "/api/v1/selftest-ghost", "Selftest: planted ghost route"},')
with open(path, "w", encoding="utf-8") as fh:
    fh.write(text)
PY
}

# 2. The spec's method flipped get -> put on an existing path: drift in BOTH
#    directions at once (code has a get the spec lost, spec has a put the code
#    never registered). This is the case the old path-only comparison missed.
break_spec_method_flip() {
    python3 - "$1/docs/openapi.yaml" <<'PY'
import re, sys
path = sys.argv[1]
with open(path, encoding="utf-8") as fh:
    text = fh.read()
text, n = re.subn(r"(?m)^(  /api/v1/jobs:\n)    get:", r"\1    put:", text)
if n != 1:
    sys.exit("break_spec_method_flip: flipped %d get: operations under /api/v1/jobs "
             "in %s — expected exactly 1" % (n, path))
with open(path, "w", encoding="utf-8") as fh:
    fh.write(text)
PY
}

# 3. A hand-written ADD_METHOD_TO the registry never saw: the route would
#    work, invisibly to --print-routes and the OpenAPI drift check.
break_rogue_controller_route() {
    python3 - "$1/src/api/HealthController.hpp" <<'PY'
import sys
path = sys.argv[1]
planted = 'ADD_METHOD_TO(HealthController::selftest_ghost, "/api/v1/selftest-ghost", Get);'
with open(path, encoding="utf-8") as fh:
    text = fh.read()
if planted in text:
    sys.exit("break_rogue_controller_route: %s already contains the planted route" % path)
# Appended after the class body — never compiled, only grepped by the gate.
with open(path, "a", encoding="utf-8") as fh:
    fh.write("\n// planted by scripts/check-selftest.sh\n" + planted + "\n")
PY
}

# 4. An unversioned /api route re-entering the registry — the ADR 0006
#    violation the versioning lint exists to stop.
break_unversioned_registry_row() {
    python3 - "$1/src/api/Endpoints.hpp" <<'PY'
import sys
path = sys.argv[1]
anchor = '{"GET", "/api/v1/jobs", "List jobs"},'
with open(path, encoding="utf-8") as fh:
    text = fh.read()
n = text.count(anchor)
if n != 1:
    sys.exit("break_unversioned_registry_row: expected exactly 1 anchor row %r in %s, found %d"
             % (anchor, path, n))
text = text.replace(
    anchor,
    anchor + '\n        {"GET", "/api/selftest-unversioned", "Selftest: planted unversioned route"},')
with open(path, "w", encoding="utf-8") as fh:
    fh.write(text)
PY
}

# 5/6. A suite name duplicated across buckets. Two cases, one per non-unit
#    bucket, because the gate USED to read only its first directory argument —
#    tests/api was silently never scanned. Case 6 is the regression test for
#    that exact bug.
_break_bucket_dup() {
    # $1 = copy root, $2 = unit suite to duplicate, $3 = file to plant it in
    python3 - "$1" "$2" "$3" <<'PY'
import glob, os, re, sys
root, suite, target = sys.argv[1], sys.argv[2], sys.argv[3]
pat = re.compile(r"TEST(_F)?\(%s," % re.escape(suite))
hits = sum(bool(pat.search(open(f, encoding="utf-8").read()))
           for f in glob.glob(os.path.join(root, "tests/unit/*.cpp")))
if hits < 1:
    sys.exit("bucket-dup mutator: suite %s no longer exists in tests/unit — "
             "pick a suite that does" % suite)
path = os.path.join(root, target)
if not os.path.isfile(path):
    sys.exit("bucket-dup mutator: %s is gone — re-point the case at a file that exists" % target)
with open(path, "a", encoding="utf-8") as fh:
    fh.write("\n// planted by scripts/check-selftest.sh — collides with tests/unit\n"
             "TEST(%s, SelftestPlantedCollision) {}\n" % suite)
PY
}
break_bucket_dup_integration() { _break_bucket_dup "$1" AuthTest tests/integration/test_auth_flow.cpp; }
break_bucket_dup_api() { _break_bucket_dup "$1" Base64Test tests/api/test_api_endpoints.cpp; }

# 7. A phantom release heading on top of the CHANGELOG: the baked
#    project(VERSION) is now stale and the gate must say so.
break_changelog_phantom_release() {
    python3 - "$1/CHANGELOG.md" <<'PY'
import sys
path = sys.argv[1]
anchor = "## [Unreleased]"
with open(path, encoding="utf-8") as fh:
    text = fh.read()
n = text.count(anchor)
if n != 1:
    sys.exit("break_changelog_phantom_release: expected exactly 1 %r heading in %s, found %d"
             % (anchor, path, n))
text = text.replace(anchor, anchor + "\n\n## [99.99.99] — 2099-01-01")
with open(path, "w", encoding="utf-8") as fh:
    fh.write(text)
PY
}

# 8. Every release heading loses its brackets. The parser now finds NOTHING —
#    and finding nothing must be a hard failure, not a shrugged-off pass:
#    "could not parse" is the difference between a gate and a no-op.
break_changelog_heading_format() {
    python3 - "$1/CHANGELOG.md" <<'PY'
import re, sys
path = sys.argv[1]
with open(path, encoding="utf-8") as fh:
    text = fh.read()
text, n = re.subn(r"(?m)^## \[(\d[0-9.]*)\]", r"## \1", text)
if n < 2:
    sys.exit("break_changelog_heading_format: de-bracketed only %d release heading(s) "
             "in %s — expected at least 2" % (n, path))
with open(path, "w", encoding="utf-8") as fh:
    fh.write(text)
PY
}

# 8b. One helm image pin left behind on the release bump — the values-stage
#     failure mode that shipped GHCR pins for tags that never existed through
#     two releases. Flips ONE of the three pins in values-stage.yaml.
break_helm_tag_drift() {
    python3 - "$1/deploy/values-worker-prod.yaml" <<'PY'
import re, sys
path = sys.argv[1]
with open(path, encoding="utf-8") as fh:
    text = fh.read()
pat = re.compile(r'(?m)^([ \t]*tag:[ \t]*")[0-9][0-9.]*(")')
if len(pat.findall(text)) != 1:
    sys.exit("break_helm_tag_drift: expected exactly 1 pinned image tag in %s, found %d"
             % (path, len(pat.findall(text))))
text, n = pat.subn(r"\g<1>9.9.9\g<2>", text, count=1)
if n != 1:
    sys.exit("break_helm_tag_drift: flipped %d pin(s) in %s — expected exactly 1" % (n, path))
with open(path, "w", encoding="utf-8") as fh:
    fh.write(text)
PY
}

# 8c. A Chart.yaml appVersion frozen at an old release — exactly how all four
#     charts sat on 1.4.0 while 1.5.x shipped (appVersion is the default image
#     tag for standalone installs and the app.kubernetes.io/version label).
break_chart_appversion_drift() {
    python3 - "$1/helm/mi-fitness-api/Chart.yaml" <<'PY'
import re, sys
path = sys.argv[1]
with open(path, encoding="utf-8") as fh:
    text = fh.read()
text, n = re.subn(r'(?m)^(appVersion:[ \t]*")[^"]*(")', r"\g<1>0.0.1\g<2>", text)
if n != 1:
    sys.exit("break_chart_appversion_drift: rewrote %d appVersion line(s) in %s — "
             "expected exactly 1" % (n, path))
with open(path, "w", encoding="utf-8") as fh:
    fh.write(text)
PY
}

# 8d. The .template-version stamp left behind on a release — a lying tarball
#     stamp hands every downstream fork's sync-upstream.sh a wrong three-way
#     patch base.
break_template_version_drift() {
    python3 - "$1/.template-version" <<'PY'
import re, sys
path = sys.argv[1]
with open(path, encoding="utf-8") as fh:
    text = fh.read()
text, n = re.subn(r"(?m)^[0-9][0-9.]*$", "0.0.9", text)
if n != 1:
    sys.exit("break_template_version_drift: rewrote %d version line(s) in %s — "
             "expected exactly 1" % (n, path))
with open(path, "w", encoding="utf-8") as fh:
    fh.write(text)
PY
}

# 11. The prod overlay's rate limiter flipped to fail-open: a Redis blip
#     would silently disable the login brute-force throttle.
break_helm_fail_open() {
    python3 - "$1/helm/mi-fitness-api/values-prod.example.yaml" <<'PY'
import re, sys
path = sys.argv[1]
with open(path, encoding="utf-8") as fh:
    text = fh.read()
text, n = re.subn(r"(?m)^(\s*)failOpen: false\b", r"\1failOpen: true", text)
if n != 1:
    sys.exit("break_helm_fail_open: flipped %d failOpen key(s) in %s — expected exactly 1"
             % (n, path))
with open(path, "w", encoding="utf-8") as fh:
    fh.write(text)
PY
}

# 12. A working JWT secret committed into the umbrella's tracked values: the
#     credential lands in the rendered Secret's data, where the env-value
#     check cannot see it — the cyber-accountant e19985b incident class. The
#     planted string is a deliberately silly non-credential; the point is
#     that the key is non-empty at all.
break_helm_committed_jwt() {
    python3 - "$1/helm/mi-fitness-api/values.yaml" <<'PY'
import re, sys
path = sys.argv[1]
with open(path, encoding="utf-8") as fh:
    text = fh.read()
text, n = re.subn(r'(?m)^(\s*)jwtSecret: ""', r'\1jwtSecret: "planted-by-check-selftest"', text, count=1)
if n != 1:
    sys.exit("break_helm_committed_jwt: planted %d jwtSecret value(s) in %s — expected exactly 1" % (n, path))
with open(path, "w", encoding="utf-8") as fh:
    fh.write(text)
PY
}

# 13. The webhooks -> email include edge resurrected — the exact edge whose
#     removal broke the email/jobs/webhooks cycle in the phase-1 hub split.
break_module_dep_cycle_edge() {
    python3 - "$1/src/webhooks/Webhooks.hpp" <<'PY'
import sys
path = sys.argv[1]
planted = '#include "email/Mailer.hpp"'
with open(path, encoding="utf-8") as fh:
    text = fh.read()
if planted in text:
    sys.exit("break_module_dep_cycle_edge: %s already includes email/Mailer.hpp — "
             "the healthy tree is not healthy" % path)
# Appended at EOF — never compiled, only grepped by the gate.
with open(path, "a", encoding="utf-8") as fh:
    fh.write("\n// planted by scripts/check-selftest.sh\n" + planted + "\n")
PY
}

# 14. A guard header quietly re-including the composition root — the include
#     that used to make every controller TU transitively pull Kafka/PayPal/
#     OTel through Guards.hpp -> Core.hpp.
break_module_dep_core_hub() {
    python3 - "$1/src/api/Guards.hpp" <<'PY'
import sys
path = sys.argv[1]
planted = '#include "core/Core.hpp"'
with open(path, encoding="utf-8") as fh:
    text = fh.read()
if planted in text:
    sys.exit("break_module_dep_core_hub: %s already includes core/Core.hpp — "
             "the healthy tree is not healthy" % path)
with open(path, "a", encoding="utf-8") as fh:
    fh.write("\n// planted by scripts/check-selftest.sh\n" + planted + "\n")
PY
}

# 15. A config knob read in code that config.json (and every other copy)
#     never heard of — the "added the key, forgot the skeletons and docs"
#     drift the config-sync gate exists to stop.
break_config_unregistered_key() {
    python3 - "$1/src/core/Core.cpp" <<'PY'
import sys
path = sys.argv[1]
planted = 'cfg.get<int>("selftest.planted_key", "SELFTEST_PLANTED_KEY", 1);'
with open(path, encoding="utf-8") as fh:
    text = fh.read()
if planted in text:
    sys.exit("break_config_unregistered_key: %s already contains the planted read" % path)
# Appended at EOF — never compiled, only parsed by the gate's extractor.
with open(path, "a", encoding="utf-8") as fh:
    fh.write("\n// planted by scripts/check-selftest.sh\n"
             "static void selftest_planted_(Config::AppConfig& cfg) { " + planted + " }\n")
PY
}

# 16. A documented env row deleted from CONFIG.md while the code still reads
#     the knob — the "full table" quietly stops being full.
break_config_doc_row_removed() {
    python3 - "$1/docs/CONFIG.md" <<'PY'
import re, sys
path = sys.argv[1]
with open(path, encoding="utf-8") as fh:
    text = fh.read()
text, n = re.subn(r"(?m)^\|\s*`MI_FITNESS_HTTP_TIMEOUT`.*\n", "", text)
if n != 1:
    sys.exit("break_config_doc_row_removed: removed %d MI_FITNESS_HTTP_TIMEOUT row(s) in %s "
             "— expected exactly 1" % (n, path))
with open(path, "w", encoding="utf-8") as fh:
    fh.write(text)
PY
}

# 17. A changelog fragment whose type is not a keep-a-changelog section —
#     assemble-changelog.sh --check must reject it BY NAME, or a typo'd
#     fragment silently never reaches the CHANGELOG at release time. The
#     mutator RENAMES the tracked README (renaming, not creating: the
#     fingerprint only watches files that existed before the mutation, so a
#     merely-added file would read as "mutator changed NOTHING").
break_changelog_fragment_type() {
    python3 - "$1" <<'PY'
import os, sys
root = sys.argv[1]
src = os.path.join(root, "changelog.d/README.md")
dst = os.path.join(root, "changelog.d/selftest-planted.badtype.md")
if not os.path.isfile(src):
    sys.exit("break_changelog_fragment_type: changelog.d/README.md is gone — "
             "re-point the mutator at a file that exists")
if os.path.exists(dst):
    sys.exit("break_changelog_fragment_type: %s already exists" % dst)
os.rename(src, dst)
PY
}

# --- run them ----------------------------------------------------------------
# Needles are LITERAL substrings of each gate's real diagnostics (grep -F).
# When a gate's wording changes, the needle changes in the same PR — that is
# the point: the selftest pins the gates to diagnostics a human can act on.

run_case registry-ghost-route check-openapi-drift.sh break_registry_ghost_route \
    "src/api/Endpoints.hpp docs/openapi.yaml" \
    'missing from openapi.yaml' \
    'get /api/v1/selftest-ghost'

run_case spec-method-flip check-openapi-drift.sh break_spec_method_flip \
    "src/api/Endpoints.hpp docs/openapi.yaml" \
    'missing from openapi.yaml' \
    'get /api/v1/jobs' \
    'not registered in Api::get_endpoints()' \
    'put /api/v1/jobs'

run_case rogue-controller-route check-routes-registered.sh break_rogue_controller_route \
    "src/api" \
    'MISSING from Api::get_endpoints() (Endpoints.hpp): GET /api/v1/selftest-ghost' \
    'Add the missing route(s)'

run_case unversioned-registry-row check-routes-registered.sh break_unversioned_registry_row \
    "src/api" \
    'UNVERSIONED API route(s)' \
    '/api/selftest-unversioned'

run_case bucket-dup-integration check-test-buckets.sh break_bucket_dup_integration \
    "tests/unit tests/integration tests/api" \
    'appear in BOTH unit and integration buckets' \
    'AuthTest' \
    'Rename one of the conflicting suites.'

run_case bucket-dup-api check-test-buckets.sh break_bucket_dup_api \
    "tests/unit tests/integration tests/api" \
    'appear in BOTH unit and integration buckets' \
    'Base64Test'

run_case changelog-phantom-release check-version-sync.sh break_changelog_phantom_release \
    "CMakeLists.txt CHANGELOG.md helm deploy .template-version project.env" \
    'version drift' \
    'newest release heading [99.99.99]'

run_case changelog-heading-format check-version-sync.sh break_changelog_heading_format \
    "CMakeLists.txt CHANGELOG.md helm deploy .template-version project.env" \
    "could not parse a released '## [x.y.z]' heading"

run_case helm-tag-drift check-version-sync.sh break_helm_tag_drift \
    "CMakeLists.txt CHANGELOG.md helm deploy .template-version project.env" \
    'helm image tag drift: deploy/values-worker-prod.yaml pins tag "9.9.9"' \
    'A stale pin deploys an old image'

run_case chart-appversion-drift check-version-sync.sh break_chart_appversion_drift \
    "CMakeLists.txt CHANGELOG.md helm deploy .template-version project.env" \
    'appVersion drift: helm/mi-fitness-api/Chart.yaml has appVersion "0.0.1"'

# В форке (project.env: TEMPLATE_FORK=1) гейт пропускает проверку штампа по
# замыслу — им владеет sync-upstream.sh, и после первого собственного релиза
# форка штамп законно отстаёт от версии. Ловить в песочнице нечего.
if grep -qs '^TEMPLATE_FORK=1' "$REPO/project.env"; then
    echo "SELFTEST SKIP [template-version-drift]: fork mode — the stamp is owned by sync-upstream.sh"
else
    run_case template-version-drift check-version-sync.sh break_template_version_drift \
        "CMakeLists.txt CHANGELOG.md helm deploy .template-version project.env" \
        '.template-version drift: the stamp says "0.0.9"' \
        'wrong three-way patch base'
fi

run_case helm-fail-open check-helm-render.sh break_helm_fail_open \
    "helm deploy" \
    'rate limiter is fail-OPEN' \
    'Set rateLimit.failOpen=false.'

run_case helm-committed-jwt check-helm-render.sh break_helm_committed_jwt \
    "helm deploy" \
    'rendered Secret carries a committed credential (jwt-secret)'

run_case module-dep-cycle-edge check-module-deps.sh break_module_dep_cycle_edge \
    "src docs/module-deps.txt" \
    'forbidden edge webhooks -> email' \
    'src/webhooks/Webhooks.hpp' \
    'utils/CurlInit.hpp'

run_case module-dep-core-hub check-module-deps.sh break_module_dep_core_hub \
    "src docs/module-deps.txt" \
    'forbidden include of the composition root: src/api/Guards.hpp includes "core/Core.hpp"' \
    'include "core/Modules.hpp"'

run_case changelog-broken-fragment 'assemble-changelog.sh --check' break_changelog_fragment_type \
    "changelog.d" \
    'changelog.d/selftest-planted.badtype.md' \
    "unknown type 'badtype'"

CONFIG_SYNC_PATHS="src config/config.json config/config.sample.json docs/CONFIG.md docs/config-sync-allowlist.txt helm/mi-fitness-api/templates/deployment.yaml helm/mi-fitness-api-worker/templates/deployment.yaml"

run_case config-unregistered-key check-config-sync.sh break_config_unregistered_key \
    "$CONFIG_SYNC_PATHS" \
    "config key 'selftest.planted_key'" \
    'missing from config/config.json' \
    'missing from config/config.sample.json' \
    "env 'SELFTEST_PLANTED_KEY'"

run_case config-stale-doc-row check-config-sync.sh break_config_doc_row_removed \
    "$CONFIG_SYNC_PATHS" \
    "env 'MI_FITNESS_HTTP_TIMEOUT' is read in src/ but not documented in docs/CONFIG.md"

# --- verdict -----------------------------------------------------------------

if [[ "$failures" -gt 0 ]]; then
    echo "check-selftest: $failures of $cases case(s) failed — the gates are NOT proven" >&2
    echo "  to catch anything. Fix this before trusting any green check-* run." >&2
    exit 1
fi
echo "check-selftest: $cases deliberate breakages, all planted, all caught, all named"
