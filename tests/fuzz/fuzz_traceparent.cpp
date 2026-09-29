/**
 * @file fuzz_traceparent.cpp
 * @brief libFuzzer harness for Observability::Trace::parse_traceparent — the
 *        W3C `traceparent` header arrives verbatim off the network on every
 *        request. Oracle: never crash/UB on arbitrary bytes; anything the
 *        parser ACCEPTS must be canonical (documented component sizes,
 *        lowercase hex) and must round-trip through format_traceparent.
 *        Compiled with src/observability/Trace.cpp only (std-only TU — see
 *        tests/fuzz/CMakeLists.txt for the no-vcpkg rationale).
 */

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "observability/Trace.hpp"

namespace {

void check(bool ok) {
    if (!ok)
        __builtin_trap();  // surfaced by libFuzzer as a crash with this input
}

bool is_lower_hex(std::string_view s) {
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    namespace T = Observability::Trace;
    const std::string_view header(reinterpret_cast<const char*>(data), size);

    if (auto parsed = T::parse_traceparent(header)) {
        // Accepted output is canonical: sizes per the W3C format, lowercase
        // hex, IDs not all-zero.
        check(parsed->trace_id.size() == 32 && is_lower_hex(parsed->trace_id));
        check(parsed->parent_id.size() == 16 && is_lower_hex(parsed->parent_id));
        check(parsed->flags.size() == 2 && is_lower_hex(parsed->flags));
        check(parsed->trace_id.find_first_not_of('0') != std::string::npos);
        check(parsed->parent_id.find_first_not_of('0') != std::string::npos);

        // format -> parse round trip reproduces the same components.
        const auto again = T::parse_traceparent(T::format_traceparent(*parsed));
        check(again.has_value());
        check(again->trace_id == parsed->trace_id && again->parent_id == parsed->parent_id &&
              again->flags == parsed->flags);
    }

    // The production entry point must always yield a usable context.
    const auto ctx = T::extract_or_generate(header);
    check(ctx.trace_id.size() == 32 && ctx.parent_id.size() == 16 && ctx.flags.size() == 2);
    return 0;
}
