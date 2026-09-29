#!/usr/bin/env bash
# Config-key drift gate: every config knob the code reads must exist in the
# committed config skeletons and the reference docs.
#
# A config key lives in 7-9 places (code, config.json, config.sample.json,
# docs/CONFIG.md, helm env lists, compose, prod overlay); until now only
# human eyes kept them in sync, and the audit that motivated this gate found
# 30+ keys the code reads that config.json never mentioned. This is the
# lightweight drift DETECTOR from the modularity plan (Phase 3) — the full
# single-registry generator is deferred; this gate just makes a forgotten
# copy a CI failure instead of a code-review coin toss.
#
# What it checks, from the code outward (the code is the source of truth):
#
#   1. Every `cfg.get<T>("json.path", "ENV_NAME", default)` /
#      `cfg.require<T>(...)` triple in src/ has its json.path present in
#      BOTH config/config.json and config/config.sample.json (placeholder
#      structure — the committed files are the discoverable key map).
#   2. Every ENV name the code reads (triples + std::getenv/env_flag_true)
#      is mentioned in docs/CONFIG.md ("the full table" is now gated).
#   3. Reverse: every ENV a docs/CONFIG.md table row documents is still
#      read somewhere in src/ — stale doc rows fail.
#   4. Helm: every all-caps `- name:` env in the mi-fitness-api / mi-fitness-api-worker
#      deployment templates is an env the code reads — a renamed or removed
#      knob can no longer leave a dead env line behind. (The full
#      helm-coverage direction — every knob reachable via helm — is the
#      deferred registry's job; every ${VAR} placeholder in config.json has
#      a `:-` default, so absence from helm is legitimate.)
#
# Reads the code can't declare in one regex-visible triple live in
# docs/config-sync-allowlist.txt with a comment each — never a silent skip.
#
# Pure bash+python3 (stdlib only), runs in well under a second, no build.

set -euo pipefail

REPO="${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

for p in src config/config.json config/config.sample.json docs/CONFIG.md docs/config-sync-allowlist.txt; do
    [ -e "$REPO/$p" ] || {
        echo "check-config-sync: $REPO/$p not found — nothing to check against" >&2
        exit 2
    }
done

python3 - "$REPO" <<'PY'
import json
import pathlib
import re
import sys

repo = pathlib.Path(sys.argv[1])

# --- 1. what the code reads --------------------------------------------------
# The one blessed read pattern (utils/Config.hpp):
#   cfg.get<T>("dot.path", "ENV_NAME", default)   [default optional]
#   cfg.require<T>("dot.path", "ENV_NAME")
# re.S because several call sites wrap the argument list across lines
# (clang-format at column 120). The json path may be "" (env-only reads like
# WORKER_TYPES); the env name is always SHOUT_CASE by convention.
TRIPLE_RE = re.compile(
    r'\.(?:get|require)<[^<>]*>\(\s*"([^"]*)"\s*,\s*"([A-Z][A-Z0-9_]*)"', re.S)
# Direct env reads that bypass Config entirely (CONFIG_FILE bootstrap,
# replica lists, standalone flags via Utils::Strings::env_flag_true).
GETENV_RE = re.compile(r'(?:std::getenv|env_flag_true)\(\s*"([A-Z][A-Z0-9_]*)"')

triples = set()   # (json_key, env) — json_key may be ""
env_only = set()
for f in sorted(list((repo / "src").rglob("*.hpp")) + list((repo / "src").rglob("*.cpp"))):
    text = f.read_text(encoding="utf-8")
    for m in TRIPLE_RE.finditer(text):
        triples.add((m.group(1), m.group(2)))
    for m in GETENV_RE.finditer(text):
        env_only.add(m.group(1))

extracted = set(triples)

# Extractor self-check: this repo has ~140 distinct triples today. If the
# regex ever rots below half of that, the gate would silently check nothing —
# the gitleaks-zero-rules disease. Fail loudly instead.
if len(extracted) < 70:
    print(f"✗ extractor found only {len(extracted)} config-read triples in src/ "
          f"(expected well over 70) — the TRIPLE_RE regex no longer matches the "
          f"codebase's read pattern. Fix scripts/check-config-sync.sh, do not trust this run.")
    sys.exit(2)

# --- 2. the allowlist: declared reads the regex can't see --------------------
#   triple <json.path> <ENV_NAME>  — read exists but is regex-invisible
#   absent-json <json.path>        — key deliberately NOT in the config jsons
allow_triples = set()
absent_json = set()
allow_path = repo / "docs" / "config-sync-allowlist.txt"
for lineno, raw in enumerate(allow_path.read_text(encoding="utf-8").splitlines(), 1):
    line = raw.split("#", 1)[0].strip()
    if not line:
        continue
    parts = line.split()
    if parts[0] == "triple" and len(parts) == 3:
        allow_triples.add((parts[1], parts[2]))
    elif parts[0] == "absent-json" and len(parts) == 2:
        absent_json.add(parts[1])
    else:
        print(f"✗ {allow_path.name}:{lineno}: malformed line: {raw.strip()!r} "
              f"(expected 'triple <json.path> <ENV>' or 'absent-json <json.path>')")
        sys.exit(2)

failures = []

# Allowlist hygiene: an entry the code now satisfies directly is rot.
for k, e in sorted(allow_triples):
    if (k, e) in extracted:
        failures.append(f"stale allowlist entry: 'triple {k} {e}' is now extracted "
                        f"directly from src/ — remove it from docs/config-sync-allowlist.txt")
triple_keys = {k for k, _ in triples | allow_triples if k}
for k in sorted(absent_json):
    if k not in triple_keys:
        failures.append(f"stale allowlist entry: 'absent-json {k}' but nothing in src/ "
                        f"reads that key — remove it from docs/config-sync-allowlist.txt")

triples |= allow_triples

# --- 3. json skeletons -------------------------------------------------------
def node_paths(node, prefix=""):
    paths = set()
    if isinstance(node, dict):
        for k, v in node.items():
            p = f"{prefix}.{k}" if prefix else k
            paths.add(p)
            paths |= node_paths(v, p)
    return paths

cfg_paths = node_paths(json.loads((repo / "config" / "config.json").read_text(encoding="utf-8")))
sample_paths = node_paths(json.loads((repo / "config" / "config.sample.json").read_text(encoding="utf-8")))

for key, env in sorted(triples):
    if not key or key in absent_json:
        continue
    if key not in cfg_paths:
        failures.append(f"config key '{key}' (env {env}) is read in src/ but missing from config/config.json")
    if key not in sample_paths:
        failures.append(f"config key '{key}' (env {env}) is read in src/ but missing from config/config.sample.json")
for key in sorted(absent_json):
    if key in cfg_paths or key in sample_paths:
        failures.append(f"allowlist says 'absent-json {key}' but the key IS present in a config json — "
                        f"remove the exemption from docs/config-sync-allowlist.txt")

# --- 4. docs/CONFIG.md, both directions --------------------------------------
doc = (repo / "docs" / "CONFIG.md").read_text(encoding="utf-8")
# Forward: any backticked mention anywhere in the file counts (some env-only
# knobs are documented in prose, not table rows).
doc_mentions = set(re.findall(r"`([A-Z][A-Z0-9_]{2,})`", doc))
# Reverse: only table rows CLAIM an env is a supported knob.
doc_table_envs = set(re.findall(r"(?m)^\|\s*`([A-Z][A-Z0-9_]+)`\s*\|", doc))

known_envs = {env for _, env in triples} | env_only

for env in sorted(known_envs):
    if env not in doc_mentions:
        failures.append(f"env '{env}' is read in src/ but not documented in docs/CONFIG.md")
for env in sorted(doc_table_envs - known_envs):
    failures.append(f"stale doc row: docs/CONFIG.md documents env '{env}' but nothing in src/ reads it")

# --- 5. helm deployments set only envs the code reads ------------------------
HELM_ENV_RE = re.compile(r"(?m)^\s*-\s*name:\s*([A-Z][A-Z0-9_]+)\s*$")
for chart in ("mi-fitness-api", "mi-fitness-api-worker"):
    dep = repo / "helm" / chart / "templates" / "deployment.yaml"
    if not dep.is_file():
        continue
    for env in sorted(set(HELM_ENV_RE.findall(dep.read_text(encoding="utf-8")))):
        if env not in known_envs:
            failures.append(f"helm/{chart}/templates/deployment.yaml sets env '{env}' "
                            f"but nothing in src/ reads it — stale or misspelled env line")

# --- verdict -----------------------------------------------------------------
if failures:
    print(f"✗ config-key drift ({len(failures)} finding(s)):")
    for f in failures:
        print(f"    {f}")
    print()
    print("Fix: a NEW key belongs in config/config.json AND config/config.sample.json")
    print("(as a '${ENV:-default}' placeholder matching the code default) AND as a row")
    print("in docs/CONFIG.md. A REMOVED/RENAMED key must also leave those copies and")
    print("the helm env lists. A read the extractor genuinely cannot see gets a")
    print("commented entry in docs/config-sync-allowlist.txt.")
    sys.exit(1)

print(f"✓ config keys in sync: {len(extracted)} extracted read triples "
      f"(+{len(allow_triples)} allowlisted), {len(known_envs)} env names covered by "
      f"config.json, config.sample.json, docs/CONFIG.md and the helm env lists")
PY
