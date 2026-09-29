#!/usr/bin/env bash
# sync-upstream.sh — pull template fixes into a fork with NO shared git history.
#
# The August 2026 downstream audit found three production forks, none of which
# had a way to take template fixes: two were degit-style copies (no upstream
# commits in their history, so `git merge upstream/master` is impossible) and
# each had independently re-fixed the same template bugs by hand. This script
# is the degit-side answer (full-history forks keep using a plain upstream
# remote + merge — see docs/UPSTREAM.md).
#
# Mechanism — release tarballs, not git history:
#   1. base   = the template release this fork was last synced to, read from
#               .template-version (or --base for old forks without the stamp)
#   2. target = --to <ver>, or the template's latest GitHub release
#   3. download both release tarballs, build one patch series with
#      `git diff --no-index base target`
#   4. seed the fork's object database with the tarball blobs, then apply the
#      patch PER FILE with `git apply -3` — three-way, so a hunk the fork has
#      edited locally becomes an in-file conflict with markers instead of a
#      silent overwrite (and a hunk the fork already carries merges to a no-op)
#   5. paths listed in .template-sync-ignore are never touched — the fork owns
#      them by definition (created with defaults on the first run)
#   6. on success .template-version is stamped to the target version; you
#      resolve conflicts, run the template's gates, and commit ONE commit.
#
# Usage (run at the FORK's repo root, on a clean tree):
#   ./scripts/sync-upstream.sh [--to <ver>] [--base <ver>] [--dry-run]
#                              [--repo <owner/name>]
#
#   --to <ver>      template version to sync TO (default: latest GitHub release)
#   --base <ver>    template version to diff FROM (default: .template-version;
#                   REQUIRED for old forks that predate the stamp file)
#   --repo <o/n>    template GitHub repo (default below; env TEMPLATE_REPO
#                   overrides the default, the flag overrides both)
#   --dry-run       print the plan (base, target, per-file actions), change nothing
set -euo pipefail

die() {
    echo "sync-upstream: $*" >&2
    exit 1
}

# The template's GitHub repo (owner/name). The default is assembled from
# split string pieces ON PURPOSE: init-project.sh's rename pass rewrites the
# joined owner and repo-name literals everywhere in the tree, and a fork whose
# sync script has forgotten where the template lives cannot sync. Do not
# "clean this up" into one literal.
_tpl_owner="moveeeax"
_tpl_name="cpp-rapid""-rest-template"
TEMPLATE_REPO="${TEMPLATE_REPO:-${_tpl_owner}/${_tpl_name}}"

BASE=""
TARGET=""
DRY_RUN=0
while [[ $# -gt 0 ]]; do
    case "$1" in
    --to)
        [[ $# -ge 2 ]] || die "--to needs a version argument"
        TARGET="$2"
        shift 2
        ;;
    --base)
        [[ $# -ge 2 ]] || die "--base needs a version argument"
        BASE="$2"
        shift 2
        ;;
    --repo)
        [[ $# -ge 2 ]] || die "--repo needs an owner/name argument"
        TEMPLATE_REPO="$2"
        shift 2
        ;;
    --dry-run | -n)
        DRY_RUN=1
        shift
        ;;
    --help | -h)
        sed -n '2,35p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *) die "unknown argument '$1' (see --help)" ;;
    esac
done

for tool in git curl tar; do
    command -v "$tool" >/dev/null 2>&1 || die "$tool required"
done

# ── Where are we ─────────────────────────────────────────────────────────────
ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" ||
    die "not inside a git checkout — run this at your fork's repo root"

# Refuse a dirty tree: conflict markers landing on top of uncommitted work are
# unrecoverable-by-git. Untracked files are fine (they can't conflict).
if [[ -n "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ]]; then
    die "working tree has uncommitted changes — commit or stash first, so a
    half-applied sync can be reverted with 'git checkout . && git reset'"
fi

# ── Resolve base ─────────────────────────────────────────────────────────────
if [[ -z "$BASE" ]]; then
    if [[ -f "$ROOT/.template-version" ]]; then
        BASE="$(tr -d '[:space:]' <"$ROOT/.template-version")"
    else
        cat >&2 <<'EOF'
sync-upstream: no .template-version in the repo root.

This fork predates the stamp file, so the base must be picked by hand ONCE:
  1. Find the template release your fork was cut from — compare the date of
     your first commit (git log --reverse --format='%ad %h' | head -1) with
     the template's release dates (its GitHub releases page / CHANGELOG.md),
     or diff a slow-moving file such as vcpkg.json against a few releases.
  2. Re-run with the base made explicit:
       ./scripts/sync-upstream.sh --base <that-version> [--to <ver>]
  3. The successful run writes .template-version, so every later sync is
     just ./scripts/sync-upstream.sh again.
EOF
        exit 1
    fi
fi

semver_or_die() {
    case "$2" in
    *[!0-9.]* | *..* | .* | *. | "") die "$1 '$2' is not plain x.y.z semver" ;;
    esac
}
semver_or_die "base version" "$BASE"

# ── Resolve target ───────────────────────────────────────────────────────────
if [[ -z "$TARGET" ]]; then
    TARGET="$(curl -fsSL --retry 2 "https://api.github.com/repos/${TEMPLATE_REPO}/releases/latest" 2>/dev/null |
        sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"v\{0,1\}\([0-9][0-9.]*\)".*/\1/p' | head -1)"
    [[ -n "$TARGET" ]] ||
        die "could not resolve the latest release of ${TEMPLATE_REPO} via the GitHub API — pass --to <version>"
fi
semver_or_die "target version" "$TARGET"

if [[ "$BASE" == "$TARGET" ]]; then
    echo "==> Already in sync: .template-version base ($BASE) == target ($TARGET). Nothing to do."
    exit 0
fi

echo "==> Syncing with template ${TEMPLATE_REPO}: $BASE -> $TARGET"

# ── Ignore list (created with defaults on the first run) ─────────────────────
IGNORE_FILE="$ROOT/.template-sync-ignore"
if [[ ! -f "$IGNORE_FILE" ]]; then
    # The chart-directory entries are split string pieces for the same reason
    # as TEMPLATE_REPO above: they must name the TEMPLATE's paths (what the
    # patch series touches), and init-project.sh's rename pass would rewrite
    # a joined literal into the fork's chart names — which no patch ever names.
    {
        echo "# Paths sync-upstream.sh never patches — the fork owns them by definition."
        echo "# One shell glob per line ('*' crosses '/'); a bare directory ignores its"
        echo "# subtree; '#' comments. Delete a line to start receiving template changes"
        echo "# for that path again. Commit this file."
        printf '%s\n' \
            ".template-version" \
            ".template-sync-ignore" \
            "project.env" \
            "CHANGELOG.md" \
            "README.md" \
            "helm/cpp""-api" \
            "helm/cpp""-worker"
    } >"$IGNORE_FILE"
    echo "==> Wrote default $IGNORE_FILE (first run) — review and commit it"
fi

is_ignored() {
    # $1 = repo-relative path. Bash 'case' globbing: '*' crosses '/'.
    local path="$1" pat
    while IFS= read -r pat; do
        pat="${pat%%#*}"
        pat="${pat%"${pat##*[![:space:]]}"}" # rtrim
        [[ -z "$pat" ]] && continue
        pat="${pat%/}"
        # shellcheck disable=SC2254  # unquoted on purpose: user-supplied glob
        case "$path" in
        $pat | $pat/*) return 0 ;;
        esac
    done <"$IGNORE_FILE"
    return 1
}

# ── Fetch and extract both release tarballs ──────────────────────────────────
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fetch_tree() {
    # $1 = version, $2 = dest dir under $TMP
    local url="https://github.com/${TEMPLATE_REPO}/archive/refs/tags/v$1.tar.gz"
    echo "==> Downloading $url"
    curl -fsSL --retry 3 -o "$TMP/$2.tar.gz" "$url" ||
        die "download failed for v$1 — does that release exist? https://github.com/${TEMPLATE_REPO}/releases"
    mkdir -p "$TMP/$2"
    tar -xzf "$TMP/$2.tar.gz" -C "$TMP/$2" --strip-components=1 ||
        die "could not extract the v$1 tarball"
}
fetch_tree "$BASE" base
fetch_tree "$TARGET" target

# ── Build the patch series ───────────────────────────────────────────────────
# One big --no-index diff between the two trees, then split per file so each
# file gets its own apply verdict (applied / conflicted / failed). Paths come
# out as a/base/<path> b/target/<path>; `git apply -p2` strips both prefixes.
set +e
(cd "$TMP" && git -c core.fileMode=false diff --no-index --binary --no-renames \
    base target >"$TMP/full.patch" 2>"$TMP/diff.err")
diff_status=$?
set -e
[[ $diff_status -le 1 ]] || die "git diff --no-index failed: $(cat "$TMP/diff.err")"

if [[ ! -s "$TMP/full.patch" ]]; then
    echo "==> No file-level differences between $BASE and $TARGET tarballs."
    if [[ $DRY_RUN -eq 0 ]]; then
        printf '%s\n' "$TARGET" >"$ROOT/.template-version"
        echo "==> .template-version -> $TARGET"
    fi
    exit 0
fi

mkdir -p "$TMP/pieces"
awk -v dir="$TMP/pieces" '
    /^diff --git / { n++ }
    n > 0 { print > sprintf("%s/%05d.patch", dir, n) }
' "$TMP/full.patch"

# Three-way apply needs the pre-image blobs the patch index lines name, and a
# degit fork's object database has never seen the template's blobs. Hash both
# tarball trees in (content-addressed: re-adding existing objects is a no-op).
find "$TMP/base" "$TMP/target" -type f -print0 |
    xargs -0 git -C "$ROOT" hash-object -w -- >/dev/null

# ── Apply per file ───────────────────────────────────────────────────────────
applied="" conflicted="" skipped="" failed=""
n_applied=0 n_conflicted=0 n_skipped=0 n_failed=0

for piece in "$TMP"/pieces/*.patch; do
    path="$(sed -n '1s|^diff --git a/base/\(.*\) b/target/.*|\1|p' "$piece")"
    [[ -n "$path" ]] || die "could not parse a path from $(head -1 "$piece")"

    if is_ignored "$path"; then
        skipped="${skipped}    $path"$'\n'
        n_skipped=$((n_skipped + 1))
        [[ $DRY_RUN -eq 1 ]] && echo "    skip   $path  (.template-sync-ignore)"
        continue
    fi

    if [[ $DRY_RUN -eq 1 ]]; then
        echo "    patch  $path"
        continue
    fi

    set +e
    out="$(git -C "$ROOT" apply -3 -p2 "$piece" 2>&1)"
    st=$?
    set -e
    if [[ $st -eq 0 ]]; then
        applied="${applied}    $path"$'\n'
        n_applied=$((n_applied + 1))
    elif printf '%s' "$out" | grep -q "with conflicts"; then
        conflicted="${conflicted}    $path"$'\n'
        n_conflicted=$((n_conflicted + 1))
    else
        failed="${failed}    $path — ${out}"$'\n'
        n_failed=$((n_failed + 1))
    fi
done

if [[ $DRY_RUN -eq 1 ]]; then
    echo ""
    echo "==> DRY RUN: plan printed above ($BASE -> $TARGET), nothing changed."
    echo "    Re-run without --dry-run to apply."
    exit 0
fi

# ── Stamp + report ───────────────────────────────────────────────────────────
printf '%s\n' "$TARGET" >"$ROOT/.template-version"

echo ""
echo "==> Sync $BASE -> $TARGET complete. Verdict:"
echo "    applied cleanly:  $n_applied file(s)"
[[ -n "$applied" ]] && printf '%s' "$applied"
echo "    conflicted:       $n_conflicted file(s) — conflict markers left in the tree"
[[ -n "$conflicted" ]] && printf '%s' "$conflicted"
echo "    skipped (ignore): $n_skipped file(s)"
[[ -n "$skipped" ]] && printf '%s' "$skipped"
echo "    failed:           $n_failed file(s)"
[[ -n "$failed" ]] && printf '%s' "$failed"
echo "    .template-version -> $TARGET"
echo ""
if [[ $n_failed -gt 0 ]]; then
    echo "A FAILED file usually means the fork deleted/renamed it — port that change"
    echo "by hand (the target tree is in the template's GitHub UI under tag v$TARGET),"
    echo "or add the path to .template-sync-ignore if the fork owns it."
fi
cat <<'EOF'
Next steps:
  1. resolve every <<<<<<< conflict (grep -rn '^<<<<<<<' — the '=======' upper
     half is YOUR fork's code, the lower half is the template's new code)
  2. read the template's CHANGELOG.md for the crossed versions — it is the
     merge guide for anything surprising
  3. run the template's gates before committing (they ship in this sync):
       make fmt && ./scripts/check-openapi-drift.sh \
         && ./scripts/check-routes-registered.sh \
         && ./scripts/check-test-buckets.sh \
         && ./scripts/check-version-sync.sh && make test
  4. commit EVERYTHING (including .template-version) as one commit, e.g.:
       git commit -am 'chore(template-sync): pull template <base> -> <target>'
EOF
if [[ $n_conflicted -gt 0 || $n_failed -gt 0 ]]; then
    exit 2
fi
