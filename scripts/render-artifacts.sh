#!/usr/bin/env bash
#
# Opt-in rendered-artifact gate: render every templates/render/<name>/
# fixtures/*.json through the FORK's renderer, extract the artifact's text
# with the FORK's extractor, and gate the result with
# scripts/check-artifact.py (content + leaked template syntax).
#
# This template repo wires nothing into CI — a fork that renders documents
# (Typst / LaTeX / HTML->PDF) enables the gate itself; docs/RENDER-GATE.md
# has the contract, the rationale and an example CI job. The precedent is the
# cyber-accountant fork's render-templates.sh: "it compiled" was never enough
# there — its v0.3.0 shipped documents that had lost their whole amounts
# column while CI stayed green — and there is no engine transcript to grep,
# because Typst clips silently with exit 0 and no log at all. Gate the
# artifact, not the engine's opinion of it.
#
# Usage:
#   RENDER_CMD='...' EXTRACT_CMD='...' ./scripts/render-artifacts.sh
#
# Contract (both commands come from the environment, both are word-split, so
# they may carry flags; wrap anything fancier in a script):
#   $RENDER_CMD  <template-dir> <fixture.json> <artifact-out>
#       renders the fixture through the template into the file <artifact-out>
#       (whatever format the fork produces), nonzero exit on failure.
#   $EXTRACT_CMD <artifact>
#       writes the artifact's extracted TEXT to stdout. `pdftotext <pdf> -`
#       via a wrapper, `lynx -dump`, `pandoc -t plain`, or plain `cat` for a
#       text artifact.
#
# Env overrides:
#   TEMPLATES_ROOT   templates root to scan  (default: templates/render)
#   CHECK_ARTIFACT   path to the gate        (default: alongside this script)
#   KEEP_ARTIFACTS   directory to keep every artifact + extracted text in,
#                    e.g. to upload from CI for human review
#                    (default: a temp dir, deleted on exit)
set -uo pipefail

TEMPLATES_ROOT="${TEMPLATES_ROOT:-templates/render}"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CHECK_ARTIFACT="${CHECK_ARTIFACT:-$SCRIPT_DIR/check-artifact.py}"

if [[ -z "${RENDER_CMD:-}" ]]; then
    echo "render-artifacts: RENDER_CMD is not set — this gate does not know how to" >&2
    echo "  render; the fork does. Set RENDER_CMD to a command invoked as" >&2
    echo "    \$RENDER_CMD <template-dir> <fixture.json> <artifact-out>" >&2
    echo "  that writes the rendered artifact to <artifact-out>. See docs/RENDER-GATE.md." >&2
    exit 2
fi
if [[ -z "${EXTRACT_CMD:-}" ]]; then
    echo "render-artifacts: EXTRACT_CMD is not set — extraction is the fork's business." >&2
    echo "  Set EXTRACT_CMD to a command invoked as" >&2
    echo "    \$EXTRACT_CMD <artifact>" >&2
    echo "  that writes the artifact's text to stdout (a pdftotext wrapper," >&2
    echo "  'lynx -dump', or plain 'cat' for a text artifact). See docs/RENDER-GATE.md." >&2
    exit 2
fi
if [[ ! -f "$CHECK_ARTIFACT" ]]; then
    echo "render-artifacts: the artifact gate is missing at '$CHECK_ARTIFACT'" >&2
    exit 2
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "render-artifacts: no python3 on PATH — scripts/check-artifact.py cannot run," >&2
    echo "  and skipping the content gate is how a lost amounts column reached" >&2
    echo "  production in the precedent fork." >&2
    exit 2
fi

if [[ -n "${KEEP_ARTIFACTS:-}" ]]; then
    WORKDIR="$KEEP_ARTIFACTS"
    mkdir -p "$WORKDIR"
else
    WORKDIR="$(mktemp -d)"
    trap 'rm -rf "$WORKDIR"' EXIT
fi

overall=0
count=0

shopt -s nullglob
for fixture in "$TEMPLATES_ROOT"/*/fixtures/*.json; do
    count=$((count + 1))
    template_dir="$(dirname "$(dirname "$fixture")")"

    # The template source is whatever single template.* file sits in the
    # template dir (template.typ, template.tex, template.html.in, ...). The
    # gate cross-checks every expected label against it, so it must be
    # unambiguous which file that is.
    source=""
    source_count=0
    for candidate in "$template_dir"/template.*; do
        source="$candidate"
        source_count=$((source_count + 1))
    done
    if [[ "$source_count" -ne 1 ]]; then
        echo "FAIL $fixture: expected exactly one template.* source in $template_dir," >&2
        echo "  found $source_count — the label anti-rot check needs to know which file" >&2
        echo "  is the template" >&2
        overall=1
        continue
    fi

    outdir="$WORKDIR/${fixture//\//_}"
    mkdir -p "$outdir"
    artifact="$outdir/artifact"

    # Deliberately word-split: RENDER_CMD/EXTRACT_CMD are a command plus
    # optional flags (the contract above), not a single pathname.
    # shellcheck disable=SC2086
    if ! $RENDER_CMD "$template_dir" "$fixture" "$artifact"; then
        echo "FAIL $fixture: RENDER_CMD failed" >&2
        overall=1
        continue
    fi
    if [[ ! -s "$artifact" ]]; then
        echo "FAIL $fixture: RENDER_CMD exited 0 but wrote no artifact at $artifact" >&2
        overall=1
        continue
    fi
    # shellcheck disable=SC2086
    if ! $EXTRACT_CMD "$artifact" > "$outdir/extracted.txt"; then
        echo "FAIL $fixture: EXTRACT_CMD failed on $artifact" >&2
        overall=1
        continue
    fi

    # The gate proper: does the artifact still say everything the fixture and
    # the template promised, without leaking template syntax?
    if ! python3 "$CHECK_ARTIFACT" "$template_dir" "$fixture" \
            "$outdir/extracted.txt" "$source"; then
        overall=1
    fi
done
shopt -u nullglob

if [[ "$count" -eq 0 ]]; then
    echo "render-artifacts: zero fixtures found under '$TEMPLATES_ROOT' — the gate" >&2
    echo "  verified nothing, and a gate that verifies nothing must not report green." >&2
    exit 1
fi

echo "render-artifacts: $count fixture(s) checked"
if [[ -n "${KEEP_ARTIFACTS:-}" ]]; then
    echo "render-artifacts: artifacts and extracted text kept under $WORKDIR"
fi
exit "$overall"
