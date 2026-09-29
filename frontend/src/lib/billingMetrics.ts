/**
 * Formatting helpers for an admin billing metrics dashboard.
 *
 * The template deliberately ships NO charting dependency, so it has no
 * metrics dashboard page — GET /api/v1/admin/billing/metrics already serves
 * the data (see docs/openapi.yaml's BillingMetricsResponse), and these pure
 * helpers are the render-boundary formatting layer for whatever charting a
 * fork wires on top (see the extension-point note in
 * pages/admin/Billing.tsx). Kept here rather than inline in a page,
 * mirroring lib/money.ts: pure and independently testable.
 */

type Period = 'day' | 'week' | 'month';

const MONTH_ABBR = [
  'Jan',
  'Feb',
  'Mar',
  'Apr',
  'May',
  'Jun',
  'Jul',
  'Aug',
  'Sep',
  'Oct',
  'Nov',
  'Dec',
];

/**
 * Revenue-chart x-axis tick label. Deterministic and UTC-based
 * (bucket_start is "ISO-8601 UTC" per docs/openapi.yaml) rather than
 * `toLocaleString` — keeps it independent of the runtime's locale/timezone,
 * which matters both for CI determinism and for this file's own test.
 */
export function formatBucketTick(iso: string, period: Period): string {
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return iso;
  if (period === 'day') return `${String(d.getUTCHours()).padStart(2, '0')}:00`;
  return `${MONTH_ABBR[d.getUTCMonth()]} ${d.getUTCDate()}`;
}

const COMPACT_USD = new Intl.NumberFormat('en-US', {
  style: 'currency',
  currency: 'USD',
  notation: 'compact',
  maximumFractionDigits: 1,
});

/**
 * Stat-tile headline value: integer cents → a dollar string, exact under
 * $1,000 and auto-compact above it ("$5.00 / $12.9K / $4.2M"). Keep the
 * exact figure reachable elsewhere (a sub-line, or the underlying table) —
 * this is a headline, never the only copy of the number.
 */
export function formatCompactUsd(cents: number): string {
  const dollars = cents / 100;
  if (Math.abs(dollars) < 1000) return `$${dollars.toFixed(2)}`;
  return COMPACT_USD.format(dollars);
}

/**
 * Same compact rule, for a value already in dollars — meant for a chart's
 * y-axis ticks (convert cents→dollars once at the render boundary for the
 * chart data, so the axis formatter works on dollars directly rather than
 * round-tripping back through cents).
 */
export function formatAxisDollars(dollars: number): string {
  if (Math.abs(dollars) < 1000) return `$${Math.round(dollars)}`;
  return COMPACT_USD.format(dollars);
}
