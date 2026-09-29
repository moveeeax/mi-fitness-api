#!/usr/bin/env python3
"""Content and leaked-syntax gate over ONE rendered artifact's extracted text.

Provenance
----------
A generalisation of the cyber-accountant fork's scripts/check-render.py — the
gate that exists because v0.3.0 of that fork shipped payslips and tax
declarations that had silently lost their entire amounts column while CI
stayed green: an overfull box is a *warning* to LaTeX, and the replacement
engine (Typst) clips silently with exit 0 and no transcript at all. The
lesson transfers to any fork that renders documents: gate the ARTIFACT, never
the engine's opinion of it.

This skeleton carries only the layers that do not depend on the artifact
being a PDF. What stayed behind in the fork, and why, is listed in
docs/RENDER-GATE.md (margin-box geometry, the rasterised strike-through ink
layer, the derived-money oracle).

Layers
------
  LAYER 1 (content)        every scalar in the fixture (except the paths
                           declared `unprinted`) and every static label in
                           expected.txt must appear in the extracted text.
  anti-rot                 every label must ALSO occur verbatim in the
                           template source, so an expectation file cannot rot
                           into asserting something the template stopped
                           printing years ago.
  LAYER 2 (leaked syntax)  no whitespace-separated token of the extracted
                           text may look like templating syntax (inja / Typst
                           / LaTeX signatures) unless it occurs inside a
                           fixture value or a declared label. Data is never
                           code: a hostile fixture that CARRIES `{{ ... }}`
                           as data is legal and must pass — syntax that
                           arrived as data is attributable, syntax with no
                           provenance escaped from the template and got
                           typeset.

Usage
-----
    check-artifact.py <template-dir> <fixture.json> <extracted.txt> <template-source>

<extracted.txt> holds the artifact's text as extracted by the FORK's own
tooling (`pdftotext`, `lynx -dump`, `pandoc -t plain`, or plain `cat` for a
text artifact). Extraction is the caller's business; this gate only reads the
result. scripts/render-artifacts.sh is the loop that produces it.

Exit status: 0 = PASS; 1 = the artifact lost content or leaked template
syntax; 2 = the gate itself could not run. 2 is a FAILED gate, not a skipped
one — silently skipping this class of check is exactly how a lost amounts
column reached production in the precedent fork.

Expectation files
-----------------
`<template-dir>/expected.txt` is REQUIRED, and `<template-dir>/fixtures/
<fixture>.expected.txt` is an optional per-fixture supplement for labels only
one branch of the template prints. One directive per line:

    # comment
    unprinted <path>        a fixture path the template deliberately never
                            prints (a control value such as a `kind`)
    known-defect <path>     content that SHOULD be printed and is not,
    known-defect <label>    because of an open bug. Excluded from the check
                            and re-announced on stderr on EVERY run, passing
                            or failing, so it cannot fade into the background.
                            Always write the reason and the follow-up in a
                            comment above it.
    <anything else>         a static label that MUST appear in the artifact

Fixture scalars: strings and integers are checked (an integer as its decimal
literal); null, booleans, floats and empty strings are skipped. A fork whose
documents print DERIVED forms (formatted money, localised dates) must add its
own oracle layer the way the precedent fork's `amount` directive does —
hand-writing the expected printed form in a fixture is how that fork shipped
`450000.00` where the law required `450 000,00`. See docs/RENDER-GATE.md.
"""

import json
import os
import re
import sys
import unicodedata

# Characters a typesetter may substitute for the ones in the fixture: curly
# and angled quotes fold to '"', every dash variant folds to '-', exotic
# spaces fold to a plain space. Nothing else is folded.
_FOLD = {c: '"' for c in "“”„‟«»″‘’‚′`´'"}
_FOLD.update({c: "-" for c in "‐‑‒–—―−"})
_SPACES = "\u00a0\u2007\u2009\u202f\u2002\u2003\u2005"

# --- LAYER 2: what "template syntax" looks like once it is on the page -------
# Applied to whole whitespace-separated TOKENS, never raw characters, because
# a token is the unit that can be attributed back to a fixture value. The list
# is short and shape-based on purpose: these sequences cannot be produced by
# typesetting ordinary prose in any of the three engine families. Forks add
# engine-specific signatures here (the precedent fork also flags a bare `[`/`]`
# because no Typst business document of its corpus prints one).
SYNTAX_SIGNATURES = (
    # inja / Jinja-family delimiters, including the comment markers.
    ("an inja delimiter", re.compile(r"\{\{|\}\}|\{%|%\}|\{#|#\}")),
    # Typst's `#` sigil followed by an identifier: `#let`, `#if`, `#d.employer`.
    # `#1` (a LaTeX macro parameter) deliberately does NOT match — a digit
    # cannot start an identifier.
    ("a Typst `#` sigil", re.compile(r"#[A-Za-z_][A-Za-z0-9_-]*")),
    # A LaTeX control sequence that reached the page instead of being run.
    # `{2,}` spares a lone trailing backslash and single escaped letters.
    ("a LaTeX control sequence", re.compile(r"\\[A-Za-z]{2,}")),
)
# Control words that leak WITHOUT their sigil because the engine consumed it
# before deciding the construct was over — Typst's `] else [` broken across
# two lines typesets a literal `else`. Kept narrow: `if`, `for`, `in`, `set`
# are ordinary English words and would put the check one line item away from
# crying wolf.
SYNTAX_KEYWORDS = frozenset((
    "else", "elif", "endif", "endfor", "endwhile",
    "let", "show", "import", "include", "context",
))
# Punctuation a leaked keyword may pick up from the text around it.
KEYWORD_TRIM = "[](){}.,;:!?\"'-/\\ "

JSON_PATH_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(\[\]|\.[A-Za-z_][A-Za-z0-9_]*)*$")


def die(msg):
    sys.stderr.write("check-artifact: %s\n" % msg)
    sys.stderr.write("check-artifact: exit 2 — the gate COULD NOT RUN, which is a "
                     "failed gate, not a skipped one\n")
    sys.exit(2)


def fold(text):
    """Normalize for comparison: NFC, fold substitutable glyphs, drop soft
    hyphens, collapse runs of whitespace that are not line breaks."""
    text = unicodedata.normalize("NFC", text)
    text = "".join(_FOLD.get(c, " " if c in _SPACES else c) for c in text)
    text = text.replace("\u00ad", "")  # soft hyphen
    return re.sub(r"[^\S\n]+", " ", text)


def collapse(text):
    return re.sub(r"\s+", " ", text).strip()


def flow_variants(text):
    """Two whole-document strings a multi-line value may be matched against:
    one that rejoins a hyphenated line break and one that keeps the hyphen."""
    joined = re.sub(r"-[ ]*\n[ ]*", "", text)
    kept = re.sub(r"-[ ]*\n[ ]*", "-", text)
    return (collapse(joined), collapse(kept))


def walk(node, path, out):
    """Flatten the fixture into (json-path, value) scalars. Array elements
    collapse to a `[]` path so a directive can address them as a set."""
    if isinstance(node, dict):
        for key, val in node.items():
            walk(val, "%s.%s" % (path, key) if path else key, out)
    elif isinstance(node, list):
        for val in node:
            walk(val, path + "[]", out)
    else:
        out.append((path, node))


def read_expectations(paths):
    """Parse expectation files into (labels, unprinted, defects, defect_labels).
    Every entry carries its "file:line" so a failure can point at the exact
    line that made the claim."""
    labels, unprinted, defects = [], set(), []
    for path in paths:
        with open(path, encoding="utf-8") as handle:
            for lineno, raw in enumerate(handle, 1):
                line = raw.strip()
                if not line or line.startswith("#"):
                    continue
                where = "%s:%d" % (path, lineno)
                if line.startswith("unprinted "):
                    unprinted.add(line[len("unprinted "):].strip())
                elif line.startswith("known-defect "):
                    defects.append((line[len("known-defect "):].strip(), where))
                else:
                    labels.append((line, where))
    # A known defect either names a fixture path — dropped from the value
    # sweep like an `unprinted` one — or quotes a static label, dropped from
    # the artifact sweep but still cross-checked against the template source,
    # since the template does try to print it.
    defect_labels = []
    for text, where in defects:
        if JSON_PATH_RE.match(text):
            unprinted.add(text)
        else:
            defect_labels.append((text, where))
            labels = [item for item in labels if item[0] != text]
    return labels, unprinted, defects, defect_labels


def check_content(extracted, text_path, fixture_path, scalars, labels, unprinted,
                  defect_labels, template_path, source):
    """LAYER 1 + anti-rot. Returns (failures, labels checked, values checked)."""
    failures = []
    folded = fold(extracted)
    flow = flow_variants(folded)
    folded_source = collapse(fold(source))

    def present(needle):
        return any(needle in variant for variant in flow)

    if not extracted.strip():
        failures.append(
            "EMPTY EXTRACTION  %s holds no text at all — either the render "
            "produced an empty artifact or EXTRACT_CMD is broken; a gate fed "
            "nothing verifies nothing" % text_path)

    for label, where in labels + defect_labels:
        needle = collapse(fold(label))
        if needle not in folded_source:
            failures.append(
                'EXPECTATION ROT  label "%s" (%s) does not occur in %s — the '
                "template stopped printing it, or the expectation is a typo"
                % (label, where, template_path))
        elif (label, where) in defect_labels:
            continue
        elif not present(needle):
            failures.append(
                'CONTENT LOST  static label "%s" (%s) is printed by the '
                "template but is not in the extracted text" % (label, where))

    checked = 0
    for path, value in scalars:
        if path in unprinted or value is None or isinstance(value, bool):
            continue
        if isinstance(value, float):
            continue  # no single canonical printed form; needs a fork oracle
        printed = str(value) if isinstance(value, int) else value
        if not printed.strip():
            continue
        checked += 1
        if not present(collapse(fold(printed))):
            failures.append(
                'CONTENT LOST  value %s = "%s" is in the fixture but not in the '
                "extracted text" % (path, printed))
    return failures, len(labels), checked


def check_leaked_syntax(extracted, fixture_path, scalars, labels, defect_labels):
    """LAYER 2. Returns (failures, tokens screened).

    A token matching a syntax signature is a finding ONLY if nothing accounts
    for it — "accounts for it" meaning the token occurs inside a fixture value
    or a declared static label. That attribution rule is the whole design: a
    hostile fixture ships `{{ malicious }}` on purpose as the test that a
    value is DATA and never source, and a check that flagged syntax on sight
    would fail it on day one and be switched off by the end of the week.
    Values are joined with NUL so a token cannot be attributed to two values
    it happens to straddle. The escape hatch is the one the rest of this file
    already has: a template that genuinely prints such a token declares it in
    expected.txt, where it is ALSO cross-checked against the template source."""
    corpus = "\x00".join(
        [collapse(fold(v)) for _p, v in scalars if isinstance(v, str)]
        + [collapse(fold(text)) for text, _w in labels + defect_labels])

    folded = fold(extracted)
    tokens = set(folded.split())
    lines = [ln.strip() for ln in folded.split("\n") if ln.strip()]

    failures = []
    for token in sorted(tokens):
        kind = None
        for name, pattern in SYNTAX_SIGNATURES:
            if pattern.search(token):
                kind = name
                break
        if kind is None and token.strip(KEYWORD_TRIM).lower() in SYNTAX_KEYWORDS:
            kind = "a control keyword"
        if kind is None:
            continue
        # `token[:-1]` is the second chance for a value the renderer
        # hyphenated at a line break: attribution must survive line breaking.
        if token in corpus or (token.endswith("-") and token[:-1] in corpus):
            continue
        context = next((ln for ln in lines if token in ln), token)
        if len(context) > 110:
            at = context.find(token)
            context = "..." + context[max(0, at - 40):at + 70] + "..."
        failures.append(
            'LEAKED SYNTAX  "%s" is printed in the artifact and is %s, but nothing '
            "in %s or in the expectation files contains it — so it did not arrive "
            "as DATA, it escaped from the template and got rendered. On the page: "
            "%r. If the template really is meant to print it, declare it as a "
            "static label; if a fixture value really carries it, the value is what "
            "has to say so" % (token, kind, fixture_path, context))
    return failures, len(tokens)


def main(argv):
    if len(argv) != 5:
        die("usage: check-artifact.py <template-dir> <fixture.json> "
            "<extracted.txt> <template-source>")
    template_dir, fixture_path, text_path, template_path = argv[1:5]
    for path in (template_dir, fixture_path, text_path, template_path):
        if not os.path.exists(path):
            die("no such path: %s" % path)

    expectation_files = [os.path.join(template_dir, "expected.txt")]
    if not os.path.exists(expectation_files[0]):
        die("%s is missing — every template must declare the static labels it "
            "prints, or the gate cannot tell a lost column from a template "
            "that never had one" % expectation_files[0])
    per_fixture = os.path.join(
        template_dir, "fixtures",
        os.path.basename(fixture_path)[:-len(".json")] + ".expected.txt")
    if os.path.exists(per_fixture):
        expectation_files.append(per_fixture)

    labels, unprinted, defects, defect_labels = read_expectations(expectation_files)

    with open(fixture_path, encoding="utf-8") as handle:
        fixture = json.load(handle)
    scalars = []
    walk(fixture, "", scalars)
    with open(text_path, encoding="utf-8") as handle:
        extracted = handle.read()
    with open(template_path, encoding="utf-8") as handle:
        source = handle.read()

    name = "%s %s" % (template_dir.rstrip("/"), os.path.basename(fixture_path))

    # Announced on every run, passing or failing: a suppression that is only
    # visible in a diff is a suppression nobody will ever remove.
    for text, where in defects:
        print('check-artifact: KNOWN DEFECT, NOT CHECKED in %s: "%s" (%s)'
              % (name, text, where), file=sys.stderr)

    content, label_count, value_count = check_content(
        extracted, text_path, fixture_path, scalars, labels, unprinted,
        defect_labels, template_path, source)
    syntax, tokens = check_leaked_syntax(
        extracted, fixture_path, scalars, labels, defect_labels)

    failures = content + syntax
    if failures:
        print("FAIL %s (%s)" % (name, text_path), file=sys.stderr)
        for failure in failures:
            print("  " + failure, file=sys.stderr)
        return 1
    print("PASS %s — %d label(s), %d value(s) present, %d token(s) screened for "
          "leaked template syntax%s"
          % (name, label_count, value_count, tokens,
             ", %d known defect(s) NOT checked" % len(defects) if defects else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
