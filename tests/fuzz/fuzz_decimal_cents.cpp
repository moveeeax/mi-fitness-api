/**
 * @file fuzz_decimal_cents.cpp
 * @brief libFuzzer harness for Billing::detail::parse_decimal_to_cents — the
 *        money parser fed by PayPal API/webhook response bodies. Oracle:
 *        arbitrary bytes either parse to a non-negative cents value that
 *        round-trips exactly through cents_to_decimal_string, or throw the
 *        documented std::runtime_error. Any OTHER escape (a different
 *        exception type, a crash, UB) is a finding. Compiled with
 *        src/billing/PayPalParse.cpp only (std-only TU, no curl).
 */

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "billing/PayPalClient.hpp"

namespace {

void check(bool ok) {
    if (!ok)
        __builtin_trap();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    namespace D = Billing::detail;
    const std::string s(reinterpret_cast<const char*>(data), size);

    try {
        const std::int64_t cents = D::parse_decimal_to_cents(s);
        // The whole point of the hand-rolled parser: money never goes
        // negative and never loses precision on a round trip.
        check(cents >= 0);
        check(D::parse_decimal_to_cents(D::cents_to_decimal_string(cents)) == cents);
    } catch (const std::runtime_error&) {
        // Documented rejection path — everything malformed lands here.
    }
    return 0;
}
