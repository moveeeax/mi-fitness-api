#!/usr/bin/env bash
# The frontend nginx config exists in TWO copies: frontend/nginx.conf (baked
# into the image; dev/compose) and the helm cpp-frontend ConfigMap (mounted
# over it in Kubernetes). Nothing compared them, so a backend route added to
# one but not the other ships a deploy where part of the public surface 404s
# — exactly how a stale chart copy took the admin UI down in a downstream
# fork (tarassov.me f4b1b class; their fix grew into
# check-public-surface-sync.sh, this is the generalized gate).
#
# The configs are ALLOWED to differ in dev-vs-k8s specifics (proxy_pass
# targets, security headers), so the assertion is scoped to what actually
# breaks routing when it drifts:
#   1. the set of `location` directives (modifier + path) must match, modulo
#      the documented helm-only exceptions below;
#   2. client_max_body_size for the upload path must match (a mismatch 413s
#      uploads in exactly one environment).
set -euo pipefail

REPO="${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
DEV_CONF="$REPO/frontend/nginx.conf"
HELM_CONF="$REPO/helm/cpp-frontend/templates/configmap.yaml"

# Helm-only locations, each with a reason. trace-ui.js is runtime SPA config
# templated from .Values.traceUi — meaningless in the compose image, which is
# built once with no cluster values to template.
HELM_ONLY_ALLOWED='^location = /trace-ui\.js$'

locations() {
    grep -E '^[[:space:]]*location[[:space:]]' "$1" |
        sed -E 's/^[[:space:]]*//; s/[[:space:]]*\{.*$//; s/[[:space:]]+/ /g' |
        sort -u
}

dev_locs="$(locations "$DEV_CONF")"
helm_locs="$(locations "$HELM_CONF" | grep -Ev "$HELM_ONLY_ALLOWED" || true)"

if [ "$dev_locs" != "$helm_locs" ]; then
    echo "✗ frontend nginx location sets drifted between the compose copy and the helm ConfigMap:" >&2
    echo "--- only in frontend/nginx.conf:" >&2
    comm -23 <(printf '%s\n' "$dev_locs") <(printf '%s\n' "$helm_locs") | sed 's/^/    /' >&2
    echo "--- only in helm/cpp-frontend/templates/configmap.yaml:" >&2
    comm -13 <(printf '%s\n' "$dev_locs") <(printf '%s\n' "$helm_locs") | sed 's/^/    /' >&2
    echo "Add the missing location to the other copy (or, for a deliberate" >&2
    echo "helm-only block, extend HELM_ONLY_ALLOWED in this script with a reason)." >&2
    exit 1
fi

body_size() { grep -oE 'client_max_body_size[[:space:]]+[0-9]+[kKmMgG]?' "$1" | awk '{print $2}' | sort -u; }
dev_body="$(body_size "$DEV_CONF")"
helm_body="$(body_size "$HELM_CONF")"
if [ "$dev_body" != "$helm_body" ]; then
    echo "✗ client_max_body_size drifted: compose='$dev_body' helm='$helm_body' — uploads will 413 in one environment only" >&2
    exit 1
fi

echo "✓ frontend nginx configs in sync ($(printf '%s\n' "$dev_locs" | grep -c .) locations, body cap $dev_body)"
