/**
 * @file TraceOtel.cpp
 * @brief Body for src/observability/TraceOtel.hpp — compiled once into
 *        app_core. Deliberately the only Trace TU that touches OpenTelemetry
 *        (see the header note); the hex_to_bytes helper lives here because
 *        this is its only caller.
 */

#include "observability/TraceOtel.hpp"

#include <cstdint>
#include <string_view>

#include <opentelemetry/nostd/span.h>

namespace Observability::Trace {

namespace {

/**
 * @brief Hex string → fixed-size byte buffer (for TraceId/SpanId).
 * @return false if the input length doesn't match or has a non-hex char.
 */
bool hex_to_bytes(std::string_view hex, uint8_t* out, size_t n) {
    if (hex.size() != n * 2)
        return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < n; ++i) {
        const int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

}  // namespace

std::optional<opentelemetry::trace::SpanContext> to_remote_span_context(const TraceContext& t) {
    uint8_t tid[16], sid[8], flags[1];
    if (!hex_to_bytes(t.trace_id, tid, 16) || !hex_to_bytes(t.parent_id, sid, 8) || !hex_to_bytes(t.flags, flags, 1)) {
        return std::nullopt;
    }
    return opentelemetry::trace::SpanContext(
        opentelemetry::trace::TraceId(opentelemetry::nostd::span<const uint8_t, 16>(tid)),
        opentelemetry::trace::SpanId(opentelemetry::nostd::span<const uint8_t, 8>(sid)),
        opentelemetry::trace::TraceFlags(flags[0]),
        /*is_remote=*/true);
}

}  // namespace Observability::Trace
