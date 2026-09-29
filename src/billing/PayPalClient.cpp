/**
 * @file PayPalClient.cpp
 * @brief Bodies for src/billing/PayPalClient.hpp — compiled once into
 *        app_core. This is the only billing TU that sees libcurl; the
 *        header no longer exposes it to including TUs.
 *
 *        The pure detail:: parser helpers (parse_decimal_to_cents,
 *        cents_to_decimal_string, find_header_ci, url_encode_segment) live
 *        in PayPalParse.cpp — a std-only TU the fuzz_decimal_cents harness
 *        (tests/fuzz) compiles without curl/spdlog/json.
 */

#include "billing/PayPalClient.hpp"

#include <cstddef>
#include <stdexcept>

#include <curl/curl.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "utils/Config.hpp"

namespace Billing {

using json = nlohmann::json;

namespace detail {

/// libcurl write callback appending into a std::string — internal to this
/// TU; nothing outside PayPalClient's own request paths takes its address.
static std::size_t curl_write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    if (out)
        out->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string describe_error_body(const std::string& resp_body) {
    constexpr std::size_t kMaxRaw = 500;
    const auto truncated = [&](const std::string& s) {
        return s.size() <= kMaxRaw ? s : s.substr(0, kMaxRaw) + "...(truncated)";
    };

    json j;
    try {
        j = json::parse(resp_body);
    } catch (const json::parse_error&) {
        return truncated(resp_body);
    }
    if (!j.is_object())
        return truncated(resp_body);

    std::string out;
    auto append_field = [&](const char* key) {
        if (j.contains(key) && j[key].is_string()) {
            if (!out.empty())
                out += " ";
            out += std::string(key) + "=" + j[key].get<std::string>();
        }
    };
    append_field("name");
    append_field("message");
    append_field("debug_id");
    append_field("error");
    append_field("error_description");
    // Orders API 422s (e.g. capturing an order the buyer never approved) put
    // the actionable code in `details[].issue`, not in `name`/`message` —
    // `name` is just the generic "UNPROCESSABLE_ENTITY" bucket. Extracting
    // this is what lets a caller (the capture flow) recognize a specific
    // issue like "ORDER_NOT_APPROVED" from the exception text and map it to
    // a 4xx instead of a bare 500.
    if (j.contains("details") && j["details"].is_array()) {
        for (const auto& d : j["details"]) {
            if (d.is_object() && d.contains("issue") && d["issue"].is_string()) {
                if (!out.empty())
                    out += " ";
                out += "issue=" + d["issue"].get<std::string>();
            }
        }
    }

    // Valid JSON, but none of the known fields — still surface something
    // rather than silently saying nothing.
    return out.empty() ? truncated(j.dump()) : out;
}

}  // namespace detail

PayPalClient::PayPalClient(PayPalClientConfig cfg) : cfg_(std::move(cfg)) {
    curl_global_init(CURL_GLOBAL_DEFAULT);  // idempotent; Storage/Mailer may have run it
    if (cfg_.timeout_sec <= 0)
        cfg_.timeout_sec = 15;
    if (cfg_.connect_timeout_sec <= 0)
        cfg_.connect_timeout_sec = 3;
}

std::string PayPalClient::base_url(const std::string& environment) {
    if (environment == "live")
        return "https://api-m.paypal.com";
    return "https://api-m.sandbox.paypal.com";
}

PayPalOrder PayPalClient::create_order(std::int64_t amount_cents,
                                       const std::string& currency,
                                       const std::string& reference,
                                       const std::string& return_url,
                                       const std::string& cancel_url) {
    json amount = json::object();
    amount["currency_code"] = currency;
    amount["value"] = detail::cents_to_decimal_string(amount_cents);

    json purchase_unit = json::object();
    purchase_unit["reference_id"] = reference;
    purchase_unit["amount"] = amount;

    json app_ctx = json::object();
    app_ctx["return_url"] = return_url;
    app_ctx["cancel_url"] = cancel_url;
    app_ctx["user_action"] = "PAY_NOW";

    json body = json::object();
    body["intent"] = "CAPTURE";
    body["purchase_units"] = json::array({purchase_unit});
    body["application_context"] = app_ctx;

    std::string resp_body;
    const long code = authorized_request("POST", "/v2/checkout/orders", body.dump(), &resp_body);
    if (code < 200 || code >= 300)
        throw std::runtime_error("paypal: create_order failed with HTTP " + std::to_string(code) + ": " +
                                 detail::describe_error_body(resp_body));

    json j;
    try {
        j = json::parse(resp_body);
    } catch (const json::parse_error&) {
        throw std::runtime_error("paypal: create_order response is not valid JSON");
    }
    if (!j.contains("id") || !j["id"].is_string() || j["id"].get<std::string>().empty())
        throw std::runtime_error("paypal: create_order response missing id");

    PayPalOrder out;
    out.order_id = j["id"].get<std::string>();
    if (j.contains("links") && j["links"].is_array()) {
        for (const auto& link : j["links"]) {
            if (link.is_object() && link.value("rel", std::string()) == "approve" && link.contains("href") &&
                link["href"].is_string()) {
                out.approve_url = link["href"].get<std::string>();
                break;
            }
        }
    }
    if (out.approve_url.empty())
        throw std::runtime_error("paypal: create_order response missing 'approve' link");
    return out;
}

PayPalCapture PayPalClient::capture_order(const std::string& order_id) {
    if (order_id.empty())
        throw std::runtime_error("paypal: capture_order: empty order_id");
    std::string resp_body;
    const long code = authorized_request(
        "POST", "/v2/checkout/orders/" + detail::url_encode_segment(order_id) + "/capture", "{}", &resp_body);
    if (code < 200 || code >= 300)
        throw std::runtime_error("paypal: capture_order failed with HTTP " + std::to_string(code) + ": " +
                                 detail::describe_error_body(resp_body));
    return parse_capture_response(resp_body);
}

bool PayPalClient::verify_webhook_signature(const std::map<std::string, std::string>& headers,
                                            const std::string& raw_body) {
    json event;
    try {
        event = json::parse(raw_body);
    } catch (const json::parse_error&) {
        spdlog::warn("paypal: webhook body is not valid JSON — treating as unverified");
        return false;
    }

    const std::string auth_algo = detail::find_header_ci(headers, "paypal-auth-algo");
    const std::string cert_url = detail::find_header_ci(headers, "paypal-cert-url");
    const std::string transmission_id = detail::find_header_ci(headers, "paypal-transmission-id");
    const std::string transmission_sig = detail::find_header_ci(headers, "paypal-transmission-sig");
    const std::string transmission_time = detail::find_header_ci(headers, "paypal-transmission-time");
    if (auth_algo.empty() || cert_url.empty() || transmission_id.empty() || transmission_sig.empty() ||
        transmission_time.empty()) {
        spdlog::warn("paypal: webhook missing one or more paypal-* headers — treating as unverified");
        return false;
    }

    json verify_req = json::object();
    verify_req["auth_algo"] = auth_algo;
    verify_req["cert_url"] = cert_url;
    verify_req["transmission_id"] = transmission_id;
    verify_req["transmission_sig"] = transmission_sig;
    verify_req["transmission_time"] = transmission_time;
    verify_req["webhook_id"] = cfg_.webhook_id;
    verify_req["webhook_event"] = event;

    std::string resp_body;
    const long code =
        authorized_request("POST", "/v1/notifications/verify-webhook-signature", verify_req.dump(), &resp_body);
    if (code < 200 || code >= 300)
        throw std::runtime_error("paypal: verify-webhook-signature failed with HTTP " + std::to_string(code) + ": " +
                                 detail::describe_error_body(resp_body));

    json j;
    try {
        j = json::parse(resp_body);
    } catch (const json::parse_error&) {
        spdlog::warn("paypal: verify-webhook-signature returned a non-JSON 2xx body — treating as unverified");
        return false;
    }
    return j.value("verification_status", std::string()) == "SUCCESS";
}

PayPalCapture PayPalClient::parse_capture_response(const std::string& json_body) {
    json j;
    try {
        j = json::parse(json_body);
    } catch (const json::parse_error& e) {
        throw std::runtime_error(std::string("paypal: malformed capture response JSON: ") + e.what());
    }

    if (!j.is_object() || !j.contains("purchase_units") || !j["purchase_units"].is_array() ||
        j["purchase_units"].empty())
        throw std::runtime_error("paypal: capture response missing purchase_units");
    const json& pu0 = j["purchase_units"][0];

    if (!pu0.is_object() || !pu0.contains("payments") || !pu0["payments"].is_object() ||
        !pu0["payments"].contains("captures") || !pu0["payments"]["captures"].is_array() ||
        pu0["payments"]["captures"].empty())
        throw std::runtime_error("paypal: capture response missing payments.captures");
    const json& cap = pu0["payments"]["captures"][0];

    if (!cap.is_object() || !cap.contains("id") || !cap["id"].is_string() || cap["id"].get<std::string>().empty())
        throw std::runtime_error("paypal: capture response missing capture id");
    if (!cap.contains("amount") || !cap["amount"].is_object() || !cap["amount"].contains("value") ||
        !cap["amount"]["value"].is_string() || !cap["amount"].contains("currency_code") ||
        !cap["amount"]["currency_code"].is_string())
        throw std::runtime_error("paypal: capture response missing amount");
    if (!cap.contains("status") || !cap["status"].is_string() || cap["status"].get<std::string>().empty())
        throw std::runtime_error("paypal: capture response missing status");

    PayPalCapture out;
    out.capture_id = cap["id"].get<std::string>();
    out.currency = cap["amount"]["currency_code"].get<std::string>();
    out.amount_cents = detail::parse_decimal_to_cents(cap["amount"]["value"].get<std::string>());
    out.status = cap["status"].get<std::string>();
    return out;
}

void PayPalClient::ensure_access_token() {
    {
        std::lock_guard<std::mutex> lock(token_mutex_);
        if (!access_token_.empty() && std::chrono::steady_clock::now() < token_expiry_)
            return;
    }

    CURL* h = curl_easy_init();
    if (!h)
        throw std::runtime_error("paypal: curl_easy_init failed");

    const std::string url = base_url(cfg_.environment) + "/v1/oauth2/token";
    const std::string post_fields = "grant_type=client_credentials";
    std::string resp_body;

    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, post_fields.c_str());
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(post_fields.size()));
    curl_easy_setopt(h, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_BASIC));
    curl_easy_setopt(h, CURLOPT_USERNAME, cfg_.client_id.c_str());
    curl_easy_setopt(h, CURLOPT_PASSWORD, cfg_.client_secret.c_str());
    curl_easy_setopt(h, CURLOPT_TIMEOUT, cfg_.timeout_sec);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, cfg_.connect_timeout_sec);
    // Multi-threaded process: without this libcurl uses SIGALRM for its
    // own timeouts and the resolver, which is not thread-safe here.
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, &detail::curl_write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &resp_body);

    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);

    const CURLcode rc = curl_easy_perform(h);
    long code = 0;
    if (rc == CURLE_OK)
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(h);

    if (rc != CURLE_OK)
        throw std::runtime_error(std::string("paypal: oauth transport error: ") + curl_easy_strerror(rc));
    if (code < 200 || code >= 300)
        throw std::runtime_error("paypal: oauth token request failed with HTTP " + std::to_string(code) + ": " +
                                 detail::describe_error_body(resp_body));

    json j;
    try {
        j = json::parse(resp_body);
    } catch (const json::parse_error&) {
        throw std::runtime_error("paypal: oauth response is not valid JSON");
    }
    if (!j.contains("access_token") || !j["access_token"].is_string() || j["access_token"].get<std::string>().empty())
        throw std::runtime_error("paypal: oauth response missing access_token");

    const std::int64_t expires_in = j.value("expires_in", static_cast<std::int64_t>(0));
    const std::string new_token = j["access_token"].get<std::string>();
    const std::int64_t safe_ttl = expires_in > 60 ? expires_in - 60 : 0;
    const auto new_expiry = std::chrono::steady_clock::now() + std::chrono::seconds(safe_ttl);

    {
        std::lock_guard<std::mutex> lock(token_mutex_);
        // Re-check: another thread may have refreshed while we were
        // doing our own round-trip outside the lock. If its token is
        // already valid, there's nothing to fix — both refreshes are
        // equally good, so just keep whichever landed first rather than
        // clobbering it with ours.
        if (access_token_.empty() || std::chrono::steady_clock::now() >= token_expiry_) {
            access_token_ = new_token;
            token_expiry_ = new_expiry;
        }
    }

    // Never log the token itself — only that one was obtained and for
    // how long it's cached.
    spdlog::info("paypal: oauth token acquired env={} expires_in={}s", cfg_.environment, expires_in);
}

long PayPalClient::authorized_request(const std::string& method,
                                      const std::string& path,
                                      const std::string& body,
                                      std::string* out_body) {
    ensure_access_token();
    std::string token_copy;
    {
        std::lock_guard<std::mutex> lock(token_mutex_);
        token_copy = access_token_;
    }

    CURL* h = curl_easy_init();
    if (!h)
        throw std::runtime_error("paypal: curl_easy_init failed");

    const std::string url = base_url(cfg_.environment) + path;
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_TIMEOUT, cfg_.timeout_sec);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, cfg_.connect_timeout_sec);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);

    if (method == "POST") {
        curl_easy_setopt(h, CURLOPT_POST, 1L);
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    } else if (method != "GET") {
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method.c_str());
    }

    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, &detail::curl_write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, out_body);

    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    // Built here and handed to curl_slist_append, which copies the
    // string internally — the token never gets logged, only ever sent
    // as a real request header. See the header's class doc comment:
    // transport errors below intentionally never echo request headers or
    // body.
    const std::string auth_header = "Authorization: Bearer " + token_copy;
    hdrs = curl_slist_append(hdrs, auth_header.c_str());
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);

    const CURLcode rc = curl_easy_perform(h);
    long code = 0;
    if (rc == CURLE_OK)
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(h);

    if (rc != CURLE_OK)
        throw std::runtime_error(std::string("paypal: ") + method + " " + path +
                                 " transport error: " + curl_easy_strerror(rc));
    return code;
}

// ── Config loading + global accessor / test seam ────────────────────────────
// The singleton lives here (was an `inline` header variable while the module
// was header-only); the header-declared free functions below are its only
// access paths, so there is nothing to declare `extern`.

namespace {
std::unique_ptr<PayPalClient> global_paypal_client = nullptr;
}  // namespace

PayPalClientConfig load_config_from_global() {
    PayPalClientConfig cfg;
    if (!Config::is_initialized())
        return cfg;
    auto& c = Config::get();
    cfg.environment = c.get<std::string>("billing.paypal.environment", "PAYPAL_ENV", "sandbox");
    cfg.client_id = c.get<std::string>("billing.paypal.client_id", "PAYPAL_CLIENT_ID", "");
    cfg.client_secret = c.get<std::string>("billing.paypal.client_secret", "PAYPAL_CLIENT_SECRET", "");
    cfg.webhook_id = c.get<std::string>("billing.paypal.webhook_id", "PAYPAL_WEBHOOK_ID", "");
    cfg.return_url = c.get<std::string>("billing.paypal.return_url", "PAYPAL_RETURN_URL", "");
    cfg.cancel_url = c.get<std::string>("billing.paypal.cancel_url", "PAYPAL_CANCEL_URL", "");
    return cfg;
}

bool is_initialized() {
    return global_paypal_client != nullptr;
}

void initialize() {
    if (global_paypal_client != nullptr)
        return;
    PayPalClientConfig cfg = load_config_from_global();
    const bool billing_on =
        Config::is_initialized() && Config::get().get<bool>("billing.enabled", "BILLING_ENABLED", false);
    if (billing_on && (cfg.client_id.empty() || cfg.client_secret.empty() || cfg.webhook_id.empty()))
        throw std::runtime_error(
            "billing.paypal.client_id / billing.paypal.client_secret / billing.paypal.webhook_id must be set "
            "when billing.enabled=true");
    global_paypal_client = std::make_unique<PayPalClient>(std::move(cfg));
}

void install_for_testing(std::unique_ptr<PayPalClient> client) {
    global_paypal_client = std::move(client);
}

void reset_for_testing() {
    global_paypal_client.reset();
}

PayPalClient& PayPalClient::get() {
    if (!global_paypal_client)
        initialize();
    return *global_paypal_client;
}

}  // namespace Billing
