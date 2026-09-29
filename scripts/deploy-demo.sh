#!/usr/bin/env bash
#
# Deploy / update the public demo environment at *.demo.tarassov.me.
#
# Stands up the cpp-env umbrella (API + worker + frontend + Mailpit) in ns
# `demo`, wired to the cluster's SHARED infra (see values-demo.yaml's header):
#   - Postgres: CNPG cluster `postgresql` in ns `db` — this script bootstraps
#     the `mi-fitness-api-demo` role + database + required extensions idempotently,
#     and mirrors the credentials into the ns-db secret `postgresql-mi-fitness-api-demo`
#     (the naming pattern every other tenant of that cluster follows);
#   - Redis: shared Sentinel Redis in ns `db`, logical DB 1 (REDIS_DB
#     isolation — needs images ≥ 1.5.4); the password is READ from the ns-db
#     secret `redis`, never generated here;
#   - Traces: cluster Tempo in ns `monitoring`, viewed via Grafana.
#
# Idempotent — re-run to update. App secrets are generated once into a
# gitignored file and reused, so the Postgres password stays stable across
# re-deploys.
#
# Usage:
#   ./scripts/deploy-demo.sh
#
# Env overrides: KUBE_CONTEXT, DEMO_NAMESPACE, DEMO_RELEASE, DEMO_ADMIN_EMAIL.
set -euo pipefail

CTX="${KUBE_CONTEXT:-YOUR_KUBE_CONTEXT}"
NS="${DEMO_NAMESPACE:-demo}"
RELEASE="${DEMO_RELEASE:-demo}"
ADMIN_EMAIL="${DEMO_ADMIN_EMAIL:-admin@demo.tarassov.me}"
# Fixed (not random) so it can be documented in the README; override if you fork.
ADMIN_PASS="${DEMO_ADMIN_PASS:-change-me-demo-pass}"

DB_NS="db"
DB_CLUSTER="postgresql"
DB_NAME="mi-fitness-api-demo"
DB_ROLE="mi-fitness-api-demo"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CHART="$ROOT/helm/cpp-env"
SECRETS="$CHART/.demo-secrets.env" # gitignored — generated once, reused

for bin in helm kubectl openssl; do
    command -v "$bin" >/dev/null 2>&1 || {
        echo "ERROR: '$bin' not found" >&2
        exit 2
    }
done

# ── App secrets: generate once, reuse afterwards (stable DB password) ──
if [[ -f "$SECRETS" ]]; then
    echo "==> Reusing demo secrets ($SECRETS)"
    # shellcheck source=/dev/null
    source "$SECRETS"
else
    echo "==> Generating fresh demo secrets → $SECRETS"
    DB_PASS="$(openssl rand -hex 24)"
    JWT_SECRET="$(openssl rand -hex 32)"
    (
        umask 177
        cat >"$SECRETS" <<EOF
DB_PASS=$DB_PASS
JWT_SECRET=$JWT_SECRET
EOF
    )
fi
: "${DB_PASS:?$SECRETS is missing DB_PASS}" "${JWT_SECRET:?$SECRETS is missing JWT_SECRET}"

# ── Shared Redis password: read, never generate ──
REDIS_PASS="$(kubectl --context "$CTX" -n "$DB_NS" get secret redis \
    -o jsonpath='{.data.redis-password}' | base64 -d)"
[[ -n "$REDIS_PASS" ]] || {
    echo "ERROR: could not read secret/redis (key redis-password) from ns $DB_NS" >&2
    exit 2
}

# ── Shared Postgres: bootstrap role + database + extensions (idempotent) ──
PRIMARY="$(kubectl --context "$CTX" -n "$DB_NS" get cluster "$DB_CLUSTER" \
    -o jsonpath='{.status.currentPrimary}')"
[[ -n "$PRIMARY" ]] || {
    echo "ERROR: CNPG cluster $DB_CLUSTER in ns $DB_NS has no currentPrimary" >&2
    exit 2
}
pg() { kubectl --context "$CTX" -n "$DB_NS" exec "$PRIMARY" -c postgres -- psql -U postgres -Atc "$1"; }

echo "==> Bootstrapping role/db '$DB_NAME' on $DB_CLUSTER (primary: $PRIMARY)"
if [[ "$(pg "SELECT 1 FROM pg_roles WHERE rolname='$DB_ROLE'")" == "1" ]]; then
    # Keep the live role in sync with the secrets file (handles rotation).
    pg "ALTER ROLE \"$DB_ROLE\" WITH LOGIN PASSWORD '$DB_PASS'" >/dev/null
else
    pg "CREATE ROLE \"$DB_ROLE\" LOGIN PASSWORD '$DB_PASS'" >/dev/null
fi
if [[ "$(pg "SELECT 1 FROM pg_database WHERE datname='$DB_NAME'")" != "1" ]]; then
    pg "CREATE DATABASE \"$DB_NAME\" OWNER \"$DB_ROLE\"" >/dev/null
fi
# Extensions the migrations need but a non-superuser owner cannot create.
for ext in pgcrypto citext; do
    kubectl --context "$CTX" -n "$DB_NS" exec "$PRIMARY" -c postgres -- \
        psql -U postgres -d "$DB_NAME" -Atc "CREATE EXTENSION IF NOT EXISTS \"$ext\"" >/dev/null
done
# Mirror the credentials in the ns-db secret, following the cluster's
# postgresql-<tenant> convention (informational for operators; the app itself
# takes the password via --set below).
kubectl --context "$CTX" -n "$DB_NS" create secret generic "postgresql-$DB_NAME" \
    --type=kubernetes.io/basic-auth \
    --from-literal=username="$DB_ROLE" --from-literal=password="$DB_PASS" \
    --dry-run=client -o yaml | kubectl --context "$CTX" apply -f - >/dev/null

echo "==> helm dependency build"
helm dependency build "$CHART" >/dev/null

echo "==> helm upgrade --install $RELEASE → ns/$NS (context $CTX)"
helm --kube-context "$CTX" upgrade --install "$RELEASE" "$CHART" \
    -n "$NS" --create-namespace \
    -f "$CHART/values-demo.yaml" \
    --set credentials.dbPassword="$DB_PASS" \
    --set credentials.redisPassword="$REDIS_PASS" \
    --set credentials.jwtSecret="$JWT_SECRET" \
    --set mi-fitness-api.externalDatabase.password="$DB_PASS" \
    --set mi-fitness-api.externalRedis.password="$REDIS_PASS" \
    --set mi-fitness-api.auth.jwtSecret="$JWT_SECRET" \
    --set mi-fitness-api-worker.externalDatabase.password="$DB_PASS" \
    --set mi-fitness-api-worker.externalRedis.password="$REDIS_PASS" \
    --set mi-fitness-api-worker.auth.jwtSecret="$JWT_SECRET" \
    --wait --timeout 10m

echo "==> Waiting for the API rollout"
kubectl --context "$CTX" -n "$NS" rollout status deploy/api --timeout=5m

# ── Demo admin + sample data (idempotent: create-admin is a no-op if it exists) ──
echo "==> Ensuring demo admin ($ADMIN_EMAIL) + seeding sample users"
# Newest RUNNING pod: right after a rollout the label still matches the old
# replica set's terminating pod, and exec-ing into it fails with "container
# not found" (bit this deploy script on the 1.5.4 rollout).
POD="$(kubectl --context "$CTX" -n "$NS" get pod -l app.kubernetes.io/name=mi-fitness-api \
    --field-selector=status.phase=Running \
    --sort-by=.metadata.creationTimestamp \
    -o jsonpath='{.items[-1:].metadata.name}')"
# config path is positional and must precede the mode flag.
kubectl --context "$CTX" -n "$NS" exec "$POD" -- \
    /app/mi_fitness_api config/config.json --create-admin "$ADMIN_EMAIL" "$ADMIN_PASS" || true
kubectl --context "$CTX" -n "$NS" exec "$POD" -- \
    /app/mi_fitness_api config/config.json --seed-fake 8 || true

cat <<EOF

============================================================
  Demo is live (give DNS + TLS a minute on first deploy):

    App      https://app.demo.tarassov.me
    API      https://api.demo.tarassov.me/healthz
    Mailbox  https://mail.demo.tarassov.me
    Traces   Grafana → Explore → Tempo (grafana.tarassov.me)

    Demo admin:  $ADMIN_EMAIL
    Password:    $ADMIN_PASS
============================================================
EOF
