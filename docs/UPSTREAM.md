# Staying in sync with the template (and giving fixes back)

Every downstream repo audited in August 2026 (three production forks) had NO
sync mechanism with this template — and each independently re-found and
re-fixed the same template bugs, while their generic fixes stranded in fork
history. This page is the contract that prevents both.

## Which fork strategy you picked decides your sync mechanics

| Strategy | How you got the code | Pulling template fixes |
|---|---|---|
| **GitHub fork / full-history clone** | `git clone` of this repo, own remote | **First choice: plain git.** `git remote add upstream <template>` + `git fetch upstream` + `git merge upstream/master` (works best if you skipped `init-project.sh` or used minimal renames). `scripts/sync-upstream.sh` also works, if you prefer release-sized steps over merge commits. |
| **degit / template-generate** (no shared history) | "Use this template", tarball import, or a squash import + `init-project.sh` | **`./scripts/sync-upstream.sh`** — merge is impossible without shared commits; the script syncs by release tarball instead (below). |

Pushing fixes back is the same in both cases — see
[the backport-candidate discipline](#pushing-back-the-backport-candidate-discipline).

## `scripts/sync-upstream.sh` — release-tarball sync for history-less forks

```bash
./scripts/sync-upstream.sh                 # sync from .template-version to the latest release
./scripts/sync-upstream.sh --to 1.6.0      # ...to a specific release
./scripts/sync-upstream.sh --dry-run       # print the plan, change nothing
./scripts/sync-upstream.sh --base 1.5.2    # old fork without a .template-version stamp
```

What it does, mechanically:

1. **Base** = `.template-version` in your repo root — a one-line stamp of the
   template release your fork last synced (written by `init-project.sh` at
   fork creation, updated by every successful sync). Forks that predate the
   stamp pass `--base` once; the run writes the stamp for next time.
2. **Target** = `--to`, or the template's latest GitHub release.
3. Downloads both release **tarballs** (no git history needed), builds a
   patch series with `git diff --no-index`, and applies it per file with
   **`git apply -3`** — three-way, so a hunk your fork edited becomes an
   in-file conflict with `<<<<<<<` markers instead of a silent overwrite,
   and a fix you already cherry-picked merges to a clean no-op.
4. Paths in **`.template-sync-ignore`** are never touched (created with
   defaults on the first run: `README.md`, `CHANGELOG.md`, `project.env`,
   the stamp files, and the template-named chart directories that
   `init-project.sh` renamed away). One shell glob per line; commit it,
   extend it with everything your fork owns outright.
5. Prints an applied / conflicted / skipped / failed verdict, stamps
   `.template-version` to the target, and exits 2 if anything needs a human.

Rules the script enforces / relies on:

- **Clean tree required.** It refuses to run over uncommitted changes — a
  bad sync must stay revertible with `git checkout . && git reset`.
- The template repo's location defaults to this template's GitHub repo and
  survives `init-project.sh`'s rename pass; override with
  `TEMPLATE_REPO=<owner>/<name>` in the environment or `--repo`.
- `.template-version` in a fork means "last template release synced" — it is
  **not** your fork's own release version. `init-project.sh` writes
  `TEMPLATE_FORK=1` into `project.env`, which makes the fork's inherited
  `release.sh` / `check-version-sync.sh` leave the stamp alone. Pre-existing
  forks adopting the script: add that line to your `project.env` yourself.

## Pulling: what to watch when you merge template updates

- **Release notes / CHANGELOG.md** are the merge guide — every behavior
  change is listed there per version. Sync release-by-release (`--to` each
  version in turn) when many releases have piled up; conflicts stay small.
- The CI gates protect you during the merge: `check-openapi-drift`,
  `check-routes-registered`, `check-test-buckets`, `check-version-sync`,
  `check-frontend-nginx-sync`, `check-helm-render` all run on your PR, so a
  half-merged invariant fails loudly instead of shipping. Run them locally
  after resolving conflicts, before committing.
- `vcpkg.json` `builtin-baseline` and `docker/Dockerfile` `ARG VCPKG_REF`
  are Renovate-owned — prefer the template's value on conflict.
- Commit the whole sync (conflict resolutions + `.template-version`) as ONE
  commit, e.g. `chore(template-sync): pull template 1.5.4 -> 1.5.5`.

## Pushing back: the backport-candidate discipline

The audit found ~15 generic fixes living only in forks (security escaping,
config coercion, connect timeouts, CI gates) — each one was a template bug
that every OTHER fork still had. To keep that from recurring:

1. When you fix something in a file that CAME FROM the template (`scripts/`,
   `.github/workflows/`, `docker/`, `helm/`, `src/` infrastructure,
   `tests/test_helpers.hpp`, …), ask: *would a fresh fork hit this too?*
2. If yes, mark the commit or PR with **`backport-candidate`** (a label, or
   just the word in the commit body) and say what's template-generic vs
   local in the PR description.
3. Periodically — or when the template maintainer runs a downstream audit —
   `git log --grep backport-candidate` is the whole worklist.
4. To land it upstream: copy the diff into a fresh branch of the template
   and PR it (a full-history fork can `cherry-pick -x` instead).

A one-line rule of thumb: **a fix without a domain noun in it belongs
upstream.** ("escape email HTML" → upstream; "escape invoice numbers in the
KZ tax PDF" → yours.)

## What the template promises in return

- Every release tarball self-identifies: `.template-version` inside it
  matches the release tag (`scripts/release.sh` bumps it,
  `check-version-sync.sh` gates it), so `sync-upstream.sh`'s three-way base
  is always honest.
- Fixes land as focused PRs with downstream-fork commit references, so your
  conflict hunks are small.
- Scaffolding (`new-*.sh`) and gates keep working on renamed forks —
  `init-project.sh` owns the rename map and regenerates `Chart.lock`.
- The image namespace in `release.yml` derives from `github.repository`, so
  a fork's releases publish under its own GHCR namespace with zero edits.
