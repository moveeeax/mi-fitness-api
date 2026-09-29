#!/usr/bin/env bash
# assemble-changelog.sh [--check] — fold changelog.d/ fragments into CHANGELOG.md.
#
# CHANGELOG.md's [Unreleased] section is the single most conflict-prone file
# in this repo (touched by a third of integrations in the blast-radius
# review): every PR appends to the same few lines. Fragments dodge the
# conflict — a PR adds its own file instead:
#
#   changelog.d/<topic>.<type>.md    type ∈ added|changed|fixed|removed|security
#
# holding ONE markdown paragraph: the bullet text WITHOUT the leading "- "
# (the assembler adds the prefix and indents continuation lines). Two
# parallel PRs then touch two different files instead of the same lines.
# changelog.d/README.md documents the format and is ignored here.
#
# Default mode: validate every fragment, fold the bullets into CHANGELOG.md's
# '## [Unreleased]' section (### subsections created in Keep-a-Changelog
# order when missing), DELETE the consumed fragments, print what happened.
# scripts/release.sh runs this automatically before retitling [Unreleased].
#
# --check: validate only (name, type, non-emptiness, single paragraph),
# touch nothing. CI runs this in the quick gate job. Fragments themselves
# stay OPTIONAL — a PR may still edit [Unreleased] directly; this gate only
# rejects fragments that exist but could not be assembled.
#
# Usage: ./scripts/assemble-changelog.sh [--check]
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO="${REPO_ROOT:-$(cd -- "$SCRIPT_DIR/.." && pwd)}"

mode=assemble
case "${1:-}" in
"") ;;
--check) mode=check ;;
*)
    echo "usage: ./scripts/assemble-changelog.sh [--check]" >&2
    exit 2
    ;;
esac

command -v python3 >/dev/null 2>&1 || {
    echo "assemble-changelog: python3 required" >&2
    exit 1
}

python3 - "$REPO" "$mode" <<'PY'
import os, re, sys

repo, mode = sys.argv[1], sys.argv[2]
frag_dir = os.path.join(repo, "changelog.d")
changelog = os.path.join(repo, "CHANGELOG.md")

VALID = ["added", "changed", "fixed", "removed", "security"]
# Keep-a-Changelog section order (the subset the fragment types cover).
CANON = ["Added", "Changed", "Removed", "Fixed", "Security"]

if not os.path.isdir(frag_dir):
    print("assemble-changelog: no changelog.d/ directory — nothing to do")
    sys.exit(0)

# --- collect + validate ------------------------------------------------------
errors = []
frags = []  # (path, section title, content lines)
for base in sorted(os.listdir(frag_dir)):
    path = os.path.join(frag_dir, base)
    rel = "changelog.d/" + base
    if not os.path.isfile(path) or base == "README.md":
        continue  # the format doc is not a fragment
    if base.startswith("."):
        errors.append((rel, "hidden files do not belong here"))
        continue
    if not base.endswith(".md"):
        errors.append((rel, "not a .md file — fragments are <topic>.<type>.md"))
        continue
    topic, dot, ftype = base[:-3].rpartition(".")
    if not dot or not topic or not ftype:
        errors.append((rel, "name must be <topic>.<type>.md (e.g. fix-login.fixed.md)"))
        continue
    if ftype not in VALID:
        errors.append((rel, "unknown type '%s' (valid: added|changed|fixed|removed|security)"
                       % ftype))
        continue
    with open(path, encoding="utf-8") as fh:
        lines = [ln.rstrip() for ln in fh.read().strip().splitlines()]
    if not lines:
        errors.append((rel, "empty fragment — write the bullet text (without the leading '- ')"))
        continue
    if lines[0].startswith(("- ", "* ")):
        errors.append((rel, "starts with a bullet prefix — the assembler adds '- ' itself"))
        continue
    if any(not ln for ln in lines):
        errors.append((rel, "must be a single paragraph (no blank lines inside the text)"))
        continue
    frags.append((path, ftype.capitalize(), lines))

if errors:
    for rel, why in errors:
        print("assemble-changelog: %s: %s" % (rel, why), file=sys.stderr)
    print("assemble-changelog: %d broken fragment(s) — fix the file name/content"
          % len(errors), file=sys.stderr)
    print("  (format reference: changelog.d/README.md)", file=sys.stderr)
    sys.exit(1)

if not frags:
    print("assemble-changelog: no fragments in changelog.d/ — nothing to %s"
          % ("check" if mode == "check" else "assemble"))
    sys.exit(0)

if mode == "check":
    print("assemble-changelog: %d fragment(s) in changelog.d/, format OK" % len(frags))
    sys.exit(0)

# --- fold into CHANGELOG.md's [Unreleased] section ---------------------------
with open(changelog, encoding="utf-8") as fh:
    lines = fh.read().splitlines()

# Find '## [Unreleased]', creating it above the newest release when missing.
unrel = [i for i, ln in enumerate(lines) if ln.strip() == "## [Unreleased]"]
if len(unrel) > 1:
    sys.exit("assemble-changelog: CHANGELOG.md has %d '## [Unreleased]' headings — "
             "expected at most 1" % len(unrel))
if unrel:
    u = unrel[0]
else:
    u = next((i for i, ln in enumerate(lines) if ln.startswith("## [")), len(lines))
    lines[u:u] = ["## [Unreleased]", ""]

# The Unreleased block runs until the next '## ' heading (or EOF).
end = next((i for i in range(u + 1, len(lines)) if lines[i].startswith("## ")), len(lines))
block = lines[u + 1:end]

# Split the block into a preamble and its '### <Type>' subsections.
pre, sections, cur = [], [], None
for ln in block:
    m = re.match(r"^###\s+(\S.*?)\s*$", ln)
    if m:
        cur = [m.group(1), []]
        sections.append(cur)
    elif cur is None:
        pre.append(ln)
    else:
        cur[1].append(ln)

def strip_blank(body):
    while body and not body[0].strip():
        body.pop(0)
    while body and not body[-1].strip():
        body.pop()
    return body

for _, body in sections:
    strip_blank(body)
strip_blank(pre)

# Append bullets to existing subsections; create missing ones (at the end of
# the block, in canonical order among themselves).
by_title = {title: body for title, body in sections}
counts = {}
for title in CANON:
    bullets = [ln for path, t, content in frags if t == title
               for ln in ["- " + content[0]] + ["  " + c for c in content[1:]]]
    if not bullets:
        continue
    counts[title] = sum(1 for _, t, _ in frags if t == title)
    if title in by_title:
        by_title[title].extend(bullets)
    else:
        sections.append([title, bullets])

new_block = []
if pre:
    new_block += [""] + pre
for title, body in sections:
    new_block += ["", "### " + title] + body
if end < len(lines):
    new_block.append("")  # one blank line before the next release heading
lines[u + 1:end] = new_block

with open(changelog, "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines) + "\n")

for path, _, _ in frags:
    os.unlink(path)

print("assemble-changelog: folded %d fragment(s) into CHANGELOG.md [Unreleased]:" % len(frags))
for title in CANON:
    if title in counts:
        print("  %-9s %d bullet(s)" % (title, counts[title]))
print("  fragments deleted — commit CHANGELOG.md together with the deletions")
PY
