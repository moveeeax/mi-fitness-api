# changelog.d — changelog fragments

`CHANGELOG.md`'s `[Unreleased]` section is the most conflict-prone file in
the repo: every PR appends to the same few lines. Instead of editing it,
drop ONE file per change here:

    <topic>.<type>.md

- `<topic>` — your branch name or a short slug (`fix-login-429`,
  `orgs-kit`). It only keeps file names unique across parallel PRs; it
  never appears in the changelog.
- `<type>` — one of `added`, `changed`, `fixed`, `removed`, `security`:
  the Keep-a-Changelog section the bullet lands in.

The file content is the bullet text: ONE markdown paragraph, WITHOUT the
leading `- ` — the assembler adds the prefix and indents continuation
lines. Backticks, links and inline markdown are fine.

Example — `changelog.d/wallet-topup.added.md`:

    PayPal checkout top-ups credited to the append-only wallet ledger
    (`src/billing/Wallet.hpp`), idempotent under webhook redelivery.

At release time `scripts/release.sh` (or a manual
`./scripts/assemble-changelog.sh`) folds every fragment into
`## [Unreleased]` — creating the `### Added` / `### Changed` / …
subsections in Keep-a-Changelog order when missing — and deletes the
consumed files; the deletions are committed with the release.

`./scripts/assemble-changelog.sh --check` validates fragment format
without touching anything; CI runs it in the quick gate job. Fragments are
OPTIONAL: editing `[Unreleased]` directly remains legal (a fragment is
preferred because it cannot conflict). This README is ignored by the
assembler.
