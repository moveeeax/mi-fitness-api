#!/usr/bin/env bash
#
# Render the production charts and assert semantic properties of
# the rendered manifests. Catches the deploy-path bug CLASS that compilation and
# unit tests can't — and that bit us repeatedly: ingress↔service port drift
# and MAIL_VIA_JOBS silently dropped because its env is gated on mail.enabled.
#
# It needs no cluster — pure `helm template` smoke test. Run via `make
# helm-validate` or the helm-charts CI job. Re-runs `helm dependency build` so a
# stale vendored subchart (charts/*.tgz are gitignored) can't skew the render.
set -euo pipefail

ROOT="${REPO_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
fail() {
    echo "FAIL: $1" >&2
    exit 1
}

command -v helm >/dev/null 2>&1 || {
    echo "helm not installed — brew install helm"
    exit 1
}
command -v yq >/dev/null 2>&1 || {
    echo "yq not installed — brew install yq"
    exit 1
}

echo "==> helm template (mi-fitness-api prod example) + security assertions"
API_CHART="$ROOT/helm/mi-fitness-api"
PROD_RENDERED="$(helm template prod-smoke "$API_CHART" -f "$API_CHART/values-prod.example.yaml")"
penv() {
    printf '%s' "$PROD_RENDERED" |
        yq "select(.kind==\"Deployment\") | .spec.template.spec.containers[0].env[] | select(.name==\"$1\") | .value"
}

[ "$(penv RATE_LIMIT_ENABLED)" = "true" ] || fail "prod overlay: RATE_LIMIT_ENABLED != true (login is unthrottled)"
[ "$(penv RATE_LIMIT_FAIL_OPEN)" = "false" ] ||
    fail "prod overlay: rate limiter is fail-OPEN — a Redis outage disables the brute-force throttle on login. Set rateLimit.failOpen=false."
[ "$(penv AUTH_MODE)" = "jwt" ] || fail "prod overlay: AUTH_MODE != jwt (endpoints would be public)"
[ "$(penv AUTH_COOKIE_SECURE)" = "true" ] || fail "prod overlay: AUTH_COOKIE_SECURE != true (__Host- cookies need Secure)"

# No plaintext secret may be baked into a TRACKED overlay — DB password / JWT
# secret must arrive via --set or external-secrets (rendered as secretKeyRef),
# so their plain .value is empty here. A non-empty value = a committed secret.
for s in DATABASE_PASSWORD JWT_SECRET; do
    [ -z "$(penv "$s")" ] || fail "prod overlay: $s carries a plaintext value — pass it via --set / external-secrets, never a tracked overlay"
done

# 8. …but that env check can only ever see the plain `.value` path, and every
#    credential in these charts reaches the container via secretKeyRef — the
#    plaintext lands in the rendered Secret's data/stringData, where nothing
#    was looking. That blind spot is how a working placeholder JWT key sat in
#    the umbrella's values with CI green until a downstream release inherited
#    it in production (cyber-accountant e19985b). Assert on the Secret payload
#    itself, over every TRACKED deployable overlay, so a credential re-entering
#    values.yaml / values-stage.yaml / values-demo.yaml fails here.
#
#    values-ci.yaml is deliberately absent from the list: its `ci-*` strings
#    are render fixtures whose whole job is to make the secretKeyRef path
#    render at all, and nothing is ever deployed from that file.
CRED_KEYS='^(password|database-password|redis-password|jwt-secret|mail-smtp-password|s3-secret-key)$'

assert_no_secret_credentials() {
    local label="$1" manifests="$2" leaked
    # Both shapes: leaf charts b64 into .data, the umbrella writes .stringData.
    leaked="$(printf '%s' "$manifests" |
        yq 'select(.kind=="Secret") | (.data // {}, .stringData // {}) | to_entries | .[] | select(.value != null and .value != "") | .key' |
        grep -E "$CRED_KEYS" | sort -u | paste -sd' ' - || true)"
    [ -z "$leaked" ] ||
        fail "$label: rendered Secret carries a committed credential (${leaked}) — tracked overlays must leave these empty and take them via --set / external-secrets"
}

assert_no_secret_credentials "mi-fitness-api values-prod.example.yaml" "$PROD_RENDERED"

WORKER_CHART="$ROOT/helm/mi-fitness-api-worker"
echo "==> helm template (worker, tracked prod values) + committed-credential assertion"
WORKER_RENDERED="$(helm template prod-smoke "$WORKER_CHART" -f "$ROOT/deploy/values-worker-prod.yaml")"
assert_no_secret_credentials "mi-fitness-api-worker values-worker-prod.yaml" "$WORKER_RENDERED"

echo "==> helm template (api, tracked prod values) + committed-credential assertion"
API_PROD_RENDERED="$(helm template prod-smoke "$API_CHART" -f "$ROOT/deploy/values-prod.yaml")"
assert_no_secret_credentials "mi-fitness-api values-prod.yaml" "$API_PROD_RENDERED"

echo "==> helm-render: all assertions passed"
