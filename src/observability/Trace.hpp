/**
 * @file Trace.hpp
 * @brief W3C Trace Context helpers — parse/format `traceparent`, generate
 *        a fresh trace ID when none is present.
 * @details Used by the HTTP pre-handling advice so every incoming request
 *          either propagates the upstream caller's trace context or gets a
 *          fresh one. The trace ID is attached to the Drogon request
 *          attributes (kTraceIdAttr) and echoed back as X-Request-Id.
 *
 *          W3C `traceparent` format:
 *            version "-" trace-id "-" parent-id "-" trace-flags
 *            00      -  32 hex    -  16 hex    -  2 hex
 *
 *          Full OTel SDK context propagation (injecting into a Span's
 *          parent context) is a natural follow-up — the skeleton here is
 *          enough for log correlation and for downstream HTTP/Kafka
 *          carriers to forward the same trace ID.
 *
 *          Bodies (the parser, hex/random helpers and the thread-local
 *          ambient traceparent) live in Trace.cpp (compiled once into
 *          app_core; ADR 0003 as amended 2026-08-22).
 *
 *          This header (and Trace.cpp) is deliberately std-only — the one
 *          OpenTelemetry-coupled helper (to_remote_span_context) lives in
 *          TraceOtel.hpp/.cpp so the parser can be compiled standalone by
 *          the libFuzzer harness in tests/fuzz (fuzz_traceparent) without
 *          the vcpkg dependency world. Keep new OTel types out of here.
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace Observability::Trace {

inline constexpr const char* kTraceIdAttr = "_trace_id";
inline constexpr const char* kSpanIdAttr = "_span_id";
inline constexpr const char* kTraceFlagsAttr = "_trace_flags";

struct TraceContext {
    std::string trace_id;   // 32 hex chars
    std::string parent_id;  // 16 hex chars
    std::string flags;      // 2 hex chars, usually "01" (sampled) or "00"
};

/**
 * @brief Parse a W3C `traceparent` header value.
 * @return nullopt if the header is malformed or the ID is all zeros.
 */
std::optional<TraceContext> parse_traceparent(std::string_view header);

/**
 * @brief Format a W3C traceparent header value.
 */
std::string format_traceparent(const TraceContext& ctx);

/**
 * @brief Produce a new trace context with freshly-generated IDs.
 */
TraceContext generate_context();

/**
 * @brief Extract a trace context from a request's traceparent header,
 *        falling back to a newly-generated one if absent/malformed.
 */
TraceContext extract_or_generate(std::string_view traceparent_header);

// to_remote_span_context (TraceContext -> remote OTel SpanContext) lives in
// TraceOtel.hpp — it is the only OTel-coupled piece of this module.

// ---------------------------------------------------------------------------
// Ambient "current request" traceparent.
//
// Set by the HTTP tracing advice for the duration of synchronous handler
// execution (handlers run synchronously on Drogon IO threads) and read by
// Jobs::submit, so a job enqueued while serving a request carries the
// originating trace context across the process boundary to the worker. Pure
// string — no OpenTelemetry coupling — so the deliberately OTel-free Jobs
// module can read it. Empty outside a request (e.g. scheduled tasks), in which
// case the worker simply starts a fresh root span as before.
// ---------------------------------------------------------------------------
std::string& current_traceparent_ref();
void set_current_traceparent(const TraceContext& ctx);
void clear_current_traceparent();
std::string current_traceparent();

}  // namespace Observability::Trace
