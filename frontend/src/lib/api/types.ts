/**
 * Flat domain-type aliases over the generated OpenAPI tree.
 *
 * openapi-typescript emits indexed types (`components['schemas']['User']`)
 * into schema.gen.ts. Call sites want flat names (`User`, `Job`, …), so
 * this module re-exports thin aliases — change a shape in
 * docs/openapi.yaml, run `npm run gen:api`, and every page picks it up.
 *
 * Everything here is now generated: the me / user-detail / roles / invite
 * response envelopes earned named `components/schemas` entries in
 * docs/openapi.yaml, so there are no hand-written envelope types left to
 * drift against the backend.
 */
import type { components } from './schema.gen';

type Schemas = components['schemas'];

export type Role = Schemas['Role'];
export type User = Schemas['User'];
export type Job = Schemas['Job'];
export type AuditEntry = Schemas['AuditEntry'];
export type AuditListResponse = Schemas['AuditListResponse'];
export type UserListResponse = Schemas['UserListResponse'];
export type JobListResponse = Schemas['JobListResponse'];
export type DlqListResponse = Schemas['DlqListResponse'];
export type JobCreate = Schemas['JobCreate'];

/** GET /api/auth/me, POST /api/auth/login, POST /api/auth/refresh — { user }. */
export type MeResponse = Schemas['MeResponse'];
/** GET/PATCH /api/admin/users/{id}, POST /api/admin/users — { data: User }. */
export type UserDetailResponse = Schemas['UserDetailResponse'];
/** GET /api/admin/roles — { data: Role[] }. */
export type RolesResponse = Schemas['RolesResponse'];
/** POST/PATCH /api/admin/roles[/{id}] — { data: Role }. */
export type RoleDetailResponse = Schemas['RoleDetailResponse'];
/** POST /api/admin/invite — { data: User, message? }. */
export type InviteResponse = Schemas['InviteResponse'];
/** Generic { message } envelope (logout, delete, …). */
export type MessageResponse = Schemas['MessageResponse'];

/**
 * Billing (wallet / PayPal top-up module). Two distinct integer units that
 * must never be mixed: `amount_cents` fields are real-world USD cents;
 * `credits` / `delta_credits` / `balance` are the internal wallet unit,
 * NEVER divided by 100 — see frontend/src/lib/money.ts for the
 * render-boundary conversion rules. credits_per_unit and the min/max bounds
 * come back on the SAME GET /billing/packages response as the package list
 * (BillingPackageListResponse).
 */
export type BillingPackage = Schemas['BillingPackage'];
export type BillingPackageListResponse = Schemas['BillingPackageListResponse'];
export type PublicWalletEntry = Schemas['PublicWalletEntry'];
export type WalletResponse = Schemas['WalletResponse'];
export type TopupResponse = Schemas['TopupResponse'];
export type CaptureResponse = Schemas['CaptureResponse'];

/** Admin billing: packages CRUD, payments list, rate/bounds settings, manual adjustments. */
export type Payment = Schemas['Payment'];
export type AdminPackageResponse = Schemas['AdminPackageResponse'];
export type AdminPackageListResponse = Schemas['AdminPackageListResponse'];
export type AdminPaymentListResponse = Schemas['AdminPaymentListResponse'];
export type BillingSettings = Schemas['BillingSettings'];
export type BillingSettingsResponse = Schemas['BillingSettingsResponse'];
export type AdjustResponse = Schemas['AdjustResponse'];
