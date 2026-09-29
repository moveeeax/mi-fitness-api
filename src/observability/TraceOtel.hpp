/**
 * @file TraceOtel.hpp
 * @brief The one OpenTelemetry-coupled Trace helper: parsed W3C traceparent
 *        -> remote OTel SpanContext.
 * @details Split out of Trace.hpp so that header (and Trace.cpp, the
 *          traceparent parser) stays std-only — the fuzz_traceparent harness
 *          in tests/fuzz compiles Trace.cpp directly, with no vcpkg
 *          dependencies. Include THIS header only where a SpanContext is
 *          actually built (the HTTP tracing advice, the job worker).
 */

#pragma once

#include <optional>

#include <opentelemetry/trace/span_context.h>

#include "observability/Trace.hpp"

namespace Observability::Trace {

/**
 * @brief Build a remote OTel SpanContext from a parsed W3C traceparent, so
 *        our server span JOINS the caller's distributed trace instead of
 *        starting an unrelated root.
 */
std::optional<opentelemetry::trace::SpanContext> to_remote_span_context(const TraceContext& t);

}  // namespace Observability::Trace
