#!/usr/bin/env bash
#
# Proves the rendered-artifact gate bites. A gate that only ever runs against
# healthy input is indistinguishable from one that returns 0 unconditionally —
# the precedent fork (cyber-accountant's check-render-selftest.sh) learned
# that twice: a path-scoped job that silently skipped, and mutators whose
# patterns stopped matching and handed the gate an UNMUTATED document, so a
# case "passed" while testing nothing. Hence, from day one here:
#
#   * a CONTROL run first: the untouched example (both fixtures, including
#     the hostile one whose `{{ malicious }}` arrives as DATA) must PASS
#     end-to-end through render-artifacts.sh, or a failure on the broken copy
#     would prove nothing;
#   * every mutator COUNTS its substitutions and exits nonzero when the count
#     is wrong, AND the harness fingerprints the file around it — a mutator
#     that changes NOTHING fails the selftest loudly;
#   * two deliberate breakages, each of which the gate must catch AND name:
#     a fixture value lost from the artifact, and a `{{ ... }}` placeholder
#     leaked into the output with no provenance in the data.
#
# Runs on a bare checkout: python3 + bash, no engine (the example's renderer
# is a file copy, its extractor is `cat`).
set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
CHECK_ARTIFACT="$SCRIPT_DIR/check-artifact.py"
EXAMPLE="$REPO_ROOT/templates/render/example"

if ! command -v python3 >/dev/null 2>&1; then
    echo "check-artifact-selftest: no python3 on PATH" >&2
    exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
failures=0

# --- control: the healthy example must PASS end-to-end ------------------------
control_out="$(RENDER_CMD="$EXAMPLE/render-stub.sh" EXTRACT_CMD=cat \
    TEMPLATES_ROOT="$REPO_ROOT/templates/render" \
    "$SCRIPT_DIR/render-artifacts.sh" 2>&1)"
control_status=$?
if [[ "$control_status" -ne 0 ]]; then
    echo "SELFTEST FAIL [control]: the gate rejects the UNMUTATED example, so a" >&2
    echo "  failure on a broken copy would prove nothing:" >&2
    printf '%s\n' "$control_out" | sed 's/^/    /' >&2
    failures=$((failures + 1))
elif ! printf '%s\n' "$control_out" | grep -q "PASS .*hostile\.json"; then
    echo "SELFTEST FAIL [control]: the hostile fixture (template syntax as DATA)" >&2
    echo "  was not checked and PASSed — the data-is-never-code property is the" >&2
    echo "  provenance rule's whole reason to exist:" >&2
    printf '%s\n' "$control_out" | sed 's/^/    /' >&2
    failures=$((failures + 1))
else
    echo "SELFTEST OK   [control]: untouched example PASSes, hostile fixture included"
fi

# --- the two breakages --------------------------------------------------------
# Each mutator edits the "extracted text" of a private copy in place, counts
# its substitutions in python3 and exits nonzero if the count is off — a
# mutator that stops mutating is a gate that stops gating.

# 1. A fixture value silently lost from the artifact (the precedent fork's
#    v0.3.0 defect shape: the document still looks right, one value is gone).
break_lost_value() {
    python3 - "$1" <<'PY'
import re, sys
path = sys.argv[1]
text = open(path, encoding="utf-8").read()
text, n = re.subn(r"INV-2026-0042", "", text)
if n != 1:
    sys.exit("break_lost_value: replaced %d of the expected 1 invoice number "
             "— the example artifact no longer has the shape this case mutates" % n)
open(path, "w", encoding="utf-8").write(text)
PY
}

# 2. An unrendered `{{ ... }}` placeholder leaked into the output. Nothing in
#    basic.json carries `{{`, so the provenance rule must flag it (while the
#    hostile fixture, which DOES carry it as data, passed in the control).
break_leaked_placeholder() {
    python3 - "$1" <<'PY'
import re, sys
path = sys.argv[1]
text = open(path, encoding="utf-8").read()
text, n = re.subn(r"Ada Lovelace", "{{ customer_name }}", text)
if n != 1:
    sys.exit("break_leaked_placeholder: replaced %d of the expected 1 customer "
             "name — the example artifact no longer has the shape this case mutates" % n)
open(path, "w", encoding="utf-8").write(text)
PY
}

# run_case <label> <mutator> <expected substring>...
run_case() {
    local label="$1" mutator="$2"
    shift 2
    local dir="$TMP/$label"
    mkdir -p "$dir"
    cp "$EXAMPLE/fixtures/basic.rendered.txt" "$dir/extracted.txt"

    local before after
    before="$(cksum < "$dir/extracted.txt")"
    if ! "$mutator" "$dir/extracted.txt"; then
        echo "SELFTEST FAIL [$label]: the mutator itself failed" >&2
        failures=$((failures + 1))
        return
    fi
    after="$(cksum < "$dir/extracted.txt")"
    if [[ "$before" == "$after" ]]; then
        echo "SELFTEST FAIL [$label]: the mutator changed NOTHING — this case would" >&2
        echo "  have re-tested the healthy artifact and reported OK. A mutator that" >&2
        echo "  stops mutating is a gate that stops gating." >&2
        failures=$((failures + 1))
        return
    fi

    local output status
    output="$(python3 "$CHECK_ARTIFACT" "$EXAMPLE" "$EXAMPLE/fixtures/basic.json" \
        "$dir/extracted.txt" "$EXAMPLE/template.txt" 2>&1)"
    status=$?
    if [[ "$status" -eq 0 ]]; then
        echo "SELFTEST FAIL [$label]: the gate PASSED a broken artifact." >&2
        printf '%s\n' "$output" | sed 's/^/    /' >&2
        failures=$((failures + 1))
        return
    fi

    local missing=0 needle
    for needle in "$@"; do
        if ! printf '%s\n' "$output" | grep -qF -- "$needle"; then
            echo "SELFTEST FAIL [$label]: the gate failed, but never said: $needle" >&2
            missing=1
        fi
    done
    if [[ "$missing" -ne 0 ]]; then
        printf '%s\n' "$output" | sed 's/^/    /' >&2
        failures=$((failures + 1))
        return
    fi
    echo "SELFTEST OK   [$label]: artifact broken -> gate exit $status, named what went wrong"
}

run_case lost-value break_lost_value \
    'CONTENT LOST' \
    'invoice_no = "INV-2026-0042"'

run_case leaked-placeholder break_leaked_placeholder \
    'LEAKED SYNTAX' \
    '"{{" is printed in the artifact' \
    'CONTENT LOST' \
    'customer_name = "Ada Lovelace"'

if [[ "$failures" -gt 0 ]]; then
    echo "check-artifact-selftest: $failures case(s) failed — the artifact gate is" >&2
    echo "  NOT proven to catch anything. Fix it before trusting a green run." >&2
    exit 1
fi
echo "check-artifact-selftest: control + 2 deliberate breakages, all applied, all caught, all named"
