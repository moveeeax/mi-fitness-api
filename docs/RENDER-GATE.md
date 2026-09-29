# Rendered-artifact gate (opt-in)

For forks that render documents — Typst, LaTeX, HTML→PDF, anything with a
template, data, and an artifact a human will read. The template repo wires
**nothing** into its own CI; a fork enables the gate itself (job fragment
below). The precedent is the cyber-accountant fork, whose v0.3.0 shipped
payslips and tax declarations that had silently lost their whole amounts
column while CI stayed green: an overfull box is a *warning* to LaTeX, and
Typst clips with exit 0 and no transcript at all. The rule that came out of
it: **gate the artifact, never the engine's opinion of it.** This skeleton
carries the PDF-independent layers of that fork's `check-render.py`; the
PDF-specific ones (margin-box geometry, the rasterised strike-through ink
layer, the derived-money oracle) stayed there — port them when your artifact
is a PDF.

## Contract

Layout, per template: `templates/render/<name>/` holding exactly one
`template.*` source, `expected.txt`, and `fixtures/*.json` (flat scalar
values). Run:

```sh
RENDER_CMD='...' EXTRACT_CMD='...' ./scripts/render-artifacts.sh
```

- `$RENDER_CMD <template-dir> <fixture.json> <artifact-out>` — the fork's
  renderer; writes the artifact to `<artifact-out>`.
- `$EXTRACT_CMD <artifact>` — the fork's text extractor; writes plain text to
  stdout (a `pdftotext "$1" -` wrapper, `lynx -dump`, or `cat` for text).

Each fixture then goes through `scripts/check-artifact.py <template-dir>
<fixture.json> <extracted.txt> <template-source>`:

1. **Content:** every fixture scalar (minus `unprinted <path>` directives)
   and every static label in `expected.txt` must appear in the extracted
   text. Every label must *also* occur verbatim in the template source, so an
   expectation file cannot rot into asserting labels the template stopped
   printing. `known-defect <path-or-label>` suppresses one check but is
   re-announced on stderr on every run, passing or failing.
2. **Leaked syntax, by provenance:** tokens that look like templating syntax
   (`{{`, `{%`, `{#`, `#let`, `#if`, `\begin`, a bare leaked `else`…) are a
   defect **only if** they occur nowhere in the fixture values or declared
   labels. **Data is never code**: `templates/render/example/fixtures/
   hostile.json` ships `{{ malicious }}` in a value on purpose and must PASS —
   syntax that arrived as data is attributable; syntax with no provenance
   escaped from the template.

Exit codes: `0` PASS, `1` defects, `2` the gate could not run — **and 2 is a
failed gate, not a skipped one.** Zero fixtures found is likewise a failure
("the gate verified nothing"). Both scripts say so explicitly.

## Rules that are not in the code but must survive in your fork

- **Pin the engine version as its own CI step.** Layout engines change layout
  across versions; a silent bump re-typesets everything in production. E.g.:
  `typst --version | grep -qE '^typst 0\.15\.1( |$)'`.
- **The oracle derives from the canon; hand-written expected forms are
  forbidden.** The precedent fork shipped `450000.00` where the law required
  `450 000,00` — fixture and PDF agreed, both wrong, because the fixture
  hand-wrote the printed money. Its fix: an `amount <path> <tiyn>` directive
  whose printed form the gate *computes* with a port of the server's own
  formatter. If your documents print derived forms (money, dates), add that
  layer; this skeleton checks strings and integer literals only.
- **A skipped job reports success.** If you path-scope the job, the filter
  must include the gate's own scripts (`scripts/check-artifact.py`,
  `scripts/render-artifacts.sh`, `scripts/check-artifact-selftest.sh`) *and
  the workflow file itself* — the precedent fork's blocks gate looked green
  twice in a row without ever executing because of exactly this.
- **The selftest is mandatory and runs in the same job.** A gate only ever
  fed healthy input is indistinguishable from `exit 0`. Mutators must count
  their substitutions; a mutator that changes nothing fails the selftest.
- **Upload the artifacts for human review** (`KEEP_ARTIFACTS=`). Text
  extraction is blind to whole defect classes (a rule struck through a name);
  until you port the ink layer, a human looking at the raster is the gate.

## Enabling in a fork's CI

```yaml
  rendered-artifacts:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      # Pin the engine (see above) and install poppler-utils/etc. here.
      - name: Prove the gate bites
        run: ./scripts/check-artifact-selftest.sh
      - name: Render every fixture and gate the artifact
        run: |
          mkdir -p render-out
          KEEP_ARTIFACTS="$PWD/render-out" \
          RENDER_CMD='./scripts/my-render-wrapper.sh' \
          EXTRACT_CMD='./scripts/my-extract-wrapper.sh' \
          ./scripts/render-artifacts.sh
      - name: Upload artifacts for human review
        if: always()
        uses: actions/upload-artifact@v4
        with: { name: rendered-artifacts, path: render-out, if-no-files-found: error }
```

`templates/render/example/` is the selftest's substrate (rendered by file
copy, extracted by `cat`). Keep it: have your `RENDER_CMD` wrapper dispatch on
the template source's extension (`.typ` → typst, `.txt` → `render-stub.sh`),
so the example keeps guarding the gate while real templates use the engine.
