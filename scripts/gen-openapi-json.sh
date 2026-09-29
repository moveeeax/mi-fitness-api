#!/usr/bin/env bash
# Convert docs/openapi.yaml into tests/e2e/openapi.gen.json — the machine-
# readable spec the e2e binary validates real HTTP response bodies against
# (tests/e2e/openapi_check.hpp).
#
# Why a COMMITTED artifact instead of parsing YAML at test time:
#   * the C++ test binary links no YAML parser, and adding one via vcpkg
#     would rebuild the entire dependency world (vetoed — see CLAUDE.md on
#     vcpkg.json);
#   * the docker test-runner image has python3 but NOT pyyaml, and the
#     drift gates deliberately avoid python+yaml so they run anywhere
#     (see scripts/check-openapi-drift.sh).
# So the conversion runs here, on a dev machine with pyyaml, and the JSON is
# committed next to the e2e sources (COPY . . puts it in the test image).
#
# Staleness cannot slip through: the JSON embeds an FNV-1a-64 hash of the
# raw YAML bytes under x-generated.source_fnv1a64, and the e2e test
# OpenApiSpec.GenJsonIsFreshAndLoadable re-hashes docs/openapi.yaml with the
# same function (mirrored in tests/e2e/openapi_check.hpp) and fails the
# suite when the stamp no longer matches. Edit docs/openapi.yaml → re-run
# this script → commit both.
#
# Usage: ./scripts/gen-openapi-json.sh
set -euo pipefail

REPO="${REPO_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
SPEC="$REPO/docs/openapi.yaml"
OUT="$REPO/tests/e2e/openapi.gen.json"

if [[ ! -f "$SPEC" ]]; then
    echo "gen-openapi-json: $SPEC not found" >&2
    exit 2
fi

python3 - "$SPEC" "$OUT" <<'PY'
import json
import sys

try:
    import yaml
except ImportError:
    sys.exit("gen-openapi-json: the python3 'yaml' module (pyyaml) is required "
             "on the machine running this generator — pip install pyyaml")

spec_path, out_path = sys.argv[1], sys.argv[2]
with open(spec_path, "rb") as fh:
    raw = fh.read()

# FNV-1a 64 over the raw YAML bytes. Mirrored byte-for-byte in
# tests/e2e/openapi_check.hpp (fnv1a64) — keep the two in sync.
h = 0xCBF29CE484222325
for b in raw:
    h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF

doc = yaml.safe_load(raw)

# Sanity: an empty/garbled conversion committed in good faith would turn the
# e2e schema validation into a rubber stamp. Refuse to write one.
paths = doc.get("paths") or {}
schemas = (doc.get("components") or {}).get("schemas") or {}
if not paths or not schemas:
    sys.exit("gen-openapi-json: conversion produced no paths or no "
             "components.schemas — refusing to write %s" % out_path)

doc["x-generated"] = {
    "by": "scripts/gen-openapi-json.sh",
    "source": "docs/openapi.yaml",
    "source_bytes": len(raw),
    "source_fnv1a64": "%016x" % h,
}

with open(out_path, "w", encoding="utf-8") as fh:
    json.dump(doc, fh, ensure_ascii=False, indent=1, sort_keys=False)
    fh.write("\n")

print("gen-openapi-json: wrote %s (%d paths, %d schemas, source fnv1a64 %016x)"
      % (out_path, len(paths), len(schemas), h))
PY
