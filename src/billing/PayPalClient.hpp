/**
 * @file PayPalClient.hpp
 * @brief PayPal REST v2 client: OAuth2 client-credentials token (cached,
 *        refreshed 60s before expiry), Orders v2 create/capture, and
 *        REST-based webhook signature verification.
 *
 * Declarations only — the bodies (and the libcurl dependency) live in
 * PayPalClient.cpp, compiled once into app_core (ADR 0003 as amended
 * 2026-08-22): including this header no longer pulls curl/json/spdlog into
 * the including TU.
 *
 * Mirrors S3Storage's libcurl usage in src/storage/Storage.hpp: one CURL easy
 * handle per call (no pooling/reuse), explicit connect+total timeouts so a
 * blackholed PayPal endpoint can't pin a Drogon IO thread forever, and
 * CURLOPT_NOSIGNAL because this runs from a multi-threaded process.
 *
 * Money-critical: the amount<->cents conversion NEVER goes through
 * double/float. PayPal amounts are decimal strings ("12.34");
 * detail::parse_decimal_to_cents splits on '.' and composes an int64 by
 * hand — see its doc comment for the exact rounding/rejection rules. This
 * matches the rest of the codebase's cents convention (Domain::Payment::
 * amount_cents, Billing::credit_capture's captured_amount_cents, …).
 *
 * Config keys (see config/config.json under billing.paypal.*):
 *   billing.paypal.environment    string  default "sandbox"  (sandbox|live)
 *   billing.paypal.client_id      string  default ""
 *   billing.paypal.client_secret  string  default ""          (never logged)
 *   billing.paypal.webhook_id     string  default ""
 *   billing.paypal.return_url     string  default ""
 *   billing.paypal.cancel_url     string  default ""
 *
 * Billing::initialize()/install_for_testing()/reset_for_testing() mirror
 * Storage::/Email::'s module test-seam shape (see the bottom of
 * src/storage/Storage.hpp): initialize() validates billing.paypal.* config
 * (throwing if billing.enabled=true and client_id/client_secret/webhook_id
 * are empty) and installs the production singleton; install_for_testing()
 * lets controller tests inject a fake/mock subclass instead of touching the
 * network. PayPalClient::get() — a class static method, per this module's
 * documented interface — lazily calls initialize() on first use if nothing
 * installed a client yet, so narrow unit tests that never boot Core keep
 * working; in production Core::initialize() calls Billing::initialize()
 * explicitly, so misconfiguration fails at boot instead of on the first
 * request.
 *
 * Every method that talks to PayPal (create_order, capture_order,
 * verify_webhook_signature's own call to PayPal) throws std::runtime_error
 * on a transport failure or a non-2xx response FROM PAYPAL — callers wrap in
 * Api::with_repo_errors, which maps an unexpected std::exception to 500 (a
 * webhook redelivery / a retried checkout is the correct outcome for a
 * transient PayPal-side or network problem). verify_webhook_signature's bool
 * return is reserved for "PayPal answered normally and the answer was
 * FAILURE / we couldn't even build a well-formed request" — seeing
 * `false` must never be confused with "PayPal is down".
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace Billing {

/// Result of PayPalClient::create_order — enough for the controller to
/// persist a Payment row (provider_order_id = order_id) and redirect the
/// browser to approve_url.
struct PayPalOrder {
    std::string order_id;
    std::string approve_url;
};

/// Result of PayPalClient::capture_order / parse_capture_response. Feeds
/// Billing::credit_capture(order_id, capture_id, amount_cents, currency)
/// directly — field names/types are chosen to match that call site verbatim.
///
/// `status` is PayPal's own capture status verbatim (e.g. "COMPLETED",
/// "PENDING", "DECLINED"). PayPal answers 2xx for ALL of these — a 2xx HTTP
/// response is not proof the money actually settled. Callers MUST check
/// `status == "COMPLETED"` before ever crediting a wallet from this struct;
/// `capture_order`'s own doc comment repeats this.
struct PayPalCapture {
    std::string capture_id;
    std::int64_t amount_cents = 0;
    std::string currency;
    std::string status;
};

namespace detail {

/**
 * @brief Decimal-string -> integer cents, WITHOUT ever going through
 *        double/float (a stod round-trip is exactly how money bugs start).
 *        Splits on '.', validates every character is a digit, and composes
 *        the int64 by hand.
 *
 * Rules — every violation throws std::runtime_error, never a silent
 * zero/wrong amount:
 *   - empty string -> throws.
 *   - a leading '+' or '-' -> throws. PayPal never signs amounts; silently
 *     accepting '-' would be a straight path to a negative-money bug.
 *   - any character outside `[0-9]` (besides a single '.') -> throws.
 *   - no '.' at all ("12") -> treated as ".00": 1200.
 *   - exactly one fractional digit ("12.3") -> right-padded: 1230.
 *   - exactly two fractional digits ("12.34") -> 1234.
 *   - MORE than two fractional digits ("12.345") -> REJECTED (throws), not
 *     truncated. Every currency this template's `amount_cents` columns
 *     support is 2-decimal (USD/EUR/…); a 3rd digit means either a currency
 *     this code doesn't actually support (JPY=0 decimals, BHD/KWD=3) or a
 *     malformed/unexpected payload. Silently truncating would round money
 *     down without anyone noticing — reject and let the caller's exception
 *     path surface it instead.
 *   - a bare trailing '.' ("12.") -> throws (empty fractional part).
 *   - an integer part longer than 15 digits -> throws (overflow guard; no
 *     real payment is anywhere near this size).
 */
std::int64_t parse_decimal_to_cents(const std::string& s);

/// Inverse of parse_decimal_to_cents, for building request bodies
/// (`amount.value` on create_order). Integer-only, same no-double rule.
std::string cents_to_decimal_string(std::int64_t cents);

/// Case-insensitive header lookup. HTTP header names are case-insensitive by
/// spec (RFC 7230 §3.2); the caller (a Drogon webhook handler) may hand us
/// headers in whatever case Drogon happened to preserve them in.
std::string find_header_ci(const std::map<std::string, std::string>& headers, const std::string& name);

/// RFC 3986 percent-encode a single path segment (order ids are opaque
/// PayPal-generated strings; this is defense-in-depth, mirrors
/// S3Storage::uri_encode_path).
std::string url_encode_segment(const std::string& s);

/// Best-effort extraction of PayPal's structured error fields from a
/// non-2xx response BODY, for inclusion in a thrown error message.
/// Orders/Payments/webhook-verify errors look like `{"name":...,
/// "message":..., "debug_id":...}`; OAuth errors look like
/// `{"error":..., "error_description":...}`. Without `debug_id`, PayPal
/// support cannot look anything up, and admins need these surfaced —
/// silently keeping only the HTTP status throws that away.
///
/// Falls back to the raw body (truncated, so one unexpectedly huge error
/// page can't make the exception message itself a problem) when the body
/// isn't JSON, isn't an object, or doesn't contain any of the known fields.
/// NEVER receives request headers or the token — this only ever sees a
/// RESPONSE body.
std::string describe_error_body(const std::string& resp_body);

}  // namespace detail

/// Construction config for PayPalClient — kept as a free struct (not a
/// nested `PayPalClient::Config`) to avoid colliding with the top-level
/// `Config::` namespace already used throughout this codebase. Mirrors
/// Email::MailerConfig's shape/naming.
struct PayPalClientConfig {
    std::string environment = "sandbox";
    std::string client_id;
    std::string client_secret;
    std::string webhook_id;
    std::string return_url;
    std::string cancel_url;
    // Whole-request and TCP-connect budgets, mirroring S3Storage::Config —
    // every call here can run inline on a Drogon IO thread, so these bound
    // how long one blackholed PayPal endpoint can pin it. No unbounded waits.
    long timeout_sec = 15;
    long connect_timeout_sec = 3;
};

class PayPalClient {
public:
    explicit PayPalClient(PayPalClientConfig cfg);

    // Virtual so tests can install a fake/mock subclass via
    // install_for_testing() below that overrides create_order/capture_order/
    // verify_webhook_signature to return canned data with no network call —
    // otherwise install_for_testing() could only swap credentials, not
    // actually keep controller tests off the network.
    virtual ~PayPalClient() = default;

    /// sandbox -> api-m.sandbox.paypal.com, live -> api-m.paypal.com, any
    /// other/unknown value -> sandbox. Fails SAFE: a config typo must never
    /// accidentally start moving real money against the live API.
    static std::string base_url(const std::string& environment);

    /// Production accessor. Lazily calls initialize() on first use if
    /// nothing installed it yet, so narrow unit tests that never boot Core
    /// keep working with zero required wiring — production boots go through
    /// Core::initialize(), which calls Billing::initialize() explicitly and
    /// fails fast at boot instead of on the first request. Throws
    /// std::runtime_error if billing.enabled=true and client_id/
    /// client_secret are empty (see initialize()).
    static PayPalClient& get();

    /// POST /v2/checkout/orders. `reference` becomes purchase_units[0].
    /// reference_id (the app's own order/payment id, for reconciliation on
    /// PayPal's dashboard). Throws std::runtime_error on transport/non-2xx
    /// or a response missing an id / an "approve" link.
    virtual PayPalOrder create_order(std::int64_t amount_cents,
                                     const std::string& currency,
                                     const std::string& reference,
                                     const std::string& return_url,
                                     const std::string& cancel_url);

    /// POST /v2/checkout/orders/{order_id}/capture. Throws
    /// std::runtime_error on transport/non-2xx or a malformed response body
    /// (see parse_capture_response). PayPal answers 2xx for a capture that is
    /// PENDING (e.g. an eCheck, or held for fraud review) or DECLINED, not
    /// only for COMPLETED — this method does NOT interpret the returned
    /// PayPalCapture::status itself; the caller MUST check it before
    /// crediting anything.
    virtual PayPalCapture capture_order(const std::string& order_id);

    /// POST /v1/notifications/verify-webhook-signature. Operates on the RAW
    /// body bytes exactly as received on the wire — @p raw_body is parsed
    /// here, once, and embedded verbatim as `webhook_event`; nothing
    /// upstream should re-serialize/re-derive it first.
    ///
    /// Returns false (does NOT throw) when:
    ///   - @p raw_body is not valid JSON, or one of the required
    ///     `paypal-*` headers is missing (case-insensitive lookup) — we
    ///     never even reach PayPal in that case;
    ///   - PayPal answers 2xx but `verification_status != "SUCCESS"`, or
    ///     answers 2xx with an unparseable body.
    /// Throws std::runtime_error (does NOT return false) on a transport
    /// failure or a non-2xx response FROM PAYPAL'S OWN API — that is a
    /// "we couldn't ask PayPal" condition, not "PayPal said no", and must
    /// not be reported to a caller as if the signature were checked and
    /// failed (a caller that conflated the two could log a benign PayPal
    /// outage as a spoofed-webhook security incident).
    virtual bool verify_webhook_signature(const std::map<std::string, std::string>& headers,
                                          const std::string& raw_body);

    /// Pure, no I/O — exposed for tests. Extracts the FIRST capture from
    /// purchase_units[0].payments.captures[0] of a PayPal v2 orders capture
    /// response, INCLUDING its `status` field verbatim (e.g. "COMPLETED",
    /// "PENDING", "DECLINED" — see PayPalCapture's doc comment; this function
    /// does not interpret it, only extracts it). Throws std::runtime_error on
    /// any missing/malformed field, including a missing/non-string status;
    /// NEVER returns a zero-amount capture for a malformed body (a bug here
    /// would silently under-credit a wallet — see parse_decimal_to_cents).
    static PayPalCapture parse_capture_response(const std::string& json_body);

private:
    /// POST /v1/oauth2/token with HTTP Basic auth (client_id/client_secret
    /// handed straight to libcurl's CURLOPT_USERNAME/PASSWORD — we never
    /// build the "Authorization: Basic ..." string ourselves, so there is
    /// nothing home-grown to accidentally log). Caches the token in-process
    /// until `expires_in - 60s`; a request that arrives inside that 60s
    /// safety margin never races an in-flight expiry.
    ///
    /// Double-checked: the fast "is my cached token still good" check runs
    /// under token_mutex_, but the OAuth network round-trip itself runs
    /// WITHOUT holding it — otherwise every concurrent billing request
    /// queues behind one slow/blackholed token endpoint for up to
    /// timeout_sec, not just the request that triggered the refresh. The
    /// lock is re-taken only to publish the result, re-checking first
    /// whether another thread already refreshed while we were making our
    /// own (possibly redundant, but harmless — both are valid) request.
    void ensure_access_token();

    /// Shared authenticated-request path for every call after OAuth:
    /// ensures a fresh token, sends `Authorization: Bearer <token>` plus a
    /// JSON body, and returns the HTTP status. Throws std::runtime_error on
    /// a transport failure; does NOT interpret the status code — every
    /// caller checks it themselves so each can phrase its own error.
    long authorized_request(const std::string& method,
                            const std::string& path,
                            const std::string& body,
                            std::string* out_body);

    PayPalClientConfig cfg_;
    std::mutex token_mutex_;
    std::string access_token_;
    std::chrono::steady_clock::time_point token_expiry_{};
};

// ── Config loading + global accessor / test seam ────────────────────────────
// Mirrors Storage::/Email::'s initialize()/install_for_testing()/
// reset_for_testing() shape (see src/storage/Storage.hpp's bottom section)
// so controller tests can inject a fake PayPalClient subclass instead of
// ever touching the network. The singleton storage itself
// (global_paypal_client) lives in PayPalClient.cpp; these functions are its
// only access paths.

PayPalClientConfig load_config_from_global();

bool is_initialized();

/// Reads billing.paypal.* (and billing.enabled) from Config and installs the
/// production singleton. Throws std::runtime_error if billing.enabled=true
/// and client_id/client_secret/webhook_id are empty — fail loud rather than
/// 500ing on the first real checkout attempt, or leaving an unset
/// webhook_id to 5xx every webhook delivery forever (mirrors
/// S3Storage::initialize throwing when storage.backend=s3 is missing its
/// endpoint/bucket). When billing is disabled (or Config was never
/// initialized — a narrow unit test that never boots Core), an
/// empty/default config is installed without validation, matching how the
/// rest of this module treats "billing off" as a no-op rather than an error.
///
/// Idempotent: a no-op if a client is already installed, whether by an
/// earlier call to this function OR by install_for_testing() — so it's safe
/// for get() to call this defensively without ever clobbering a test
/// double a caller already installed.
void initialize();

/// Test seam: install a fake/mock PayPalClient (a test-only subclass
/// overriding create_order/capture_order/verify_webhook_signature) so
/// controller tests can exercise the checkout/capture/webhook flow with no
/// network access. Mirrors Storage::install_for_testing.
void install_for_testing(std::unique_ptr<PayPalClient> client);

void reset_for_testing();

}  // namespace Billing
