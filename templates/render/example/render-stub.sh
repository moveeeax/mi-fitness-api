#!/usr/bin/env bash
# Stand-in RENDER_CMD for the example template: "rendering" is copying the
# prebaked rendered text that sits next to the fixture. No engine involved —
# this is what lets scripts/check-artifact-selftest.sh exercise the whole
# gate on any machine, which is the example's entire job. A fork with a real
# engine keeps this pattern for the example (dispatch on the template source
# extension in its RENDER_CMD wrapper) or drops templates/render/example/
# together with the selftest's substrate — see docs/RENDER-GATE.md.
#
# Invoked per the render-artifacts.sh contract:
#   render-stub.sh <template-dir> <fixture.json> <artifact-out>
set -euo pipefail
template_dir="$1"
fixture="$2"
out="$3"
name="$(basename "$fixture" .json)"
cp "$template_dir/fixtures/$name.rendered.txt" "$out"
