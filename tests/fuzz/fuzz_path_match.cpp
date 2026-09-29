/**
 * @file fuzz_path_match.cpp
 * @brief libFuzzer harness for the request-path matchers every middleware
 *        runs on raw request paths: Utils::Strings::path_is_public (+ the
 *        CSV split/merge that builds its set) and
 *        Api::normalize_path_for_metrics. Input layout: bytes up to the
 *        first '\n' are an extra public-paths CSV (exercises
 *        split_csv_vec/merge_csv_sets on hostile config), the rest is the
 *        request path. Oracle: never crash/UB; normalization always yields
 *        a rooted path and is idempotent (a normalized path re-normalizes
 *        to itself — if it didn't, metrics labels would drift). Compiled
 *        with src/utils/Strings.cpp + the header-only PathNormalize.hpp.
 */

#include <cstddef>
#include <cstdint>
#include <string>

#include "api/PathNormalize.hpp"
#include "utils/Strings.hpp"

namespace {

void check(bool ok) {
    if (!ok)
        __builtin_trap();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    namespace S = Utils::Strings;
    const std::string input(reinterpret_cast<const char*>(data), size);

    const auto nl = input.find('\n');
    const std::string extra_csv = (nl == std::string::npos) ? std::string() : input.substr(0, nl);
    const std::string path = (nl == std::string::npos) ? input : input.substr(nl + 1);

    const auto public_paths = S::merge_csv_sets(S::kDefaultPublicPathsCsv, extra_csv);
    (void)S::path_is_public(public_paths, path);

    const std::string norm = Api::normalize_path_for_metrics(path);
    check(!norm.empty() && norm.front() == '/');
    check(Api::normalize_path_for_metrics(norm) == norm);
    return 0;
}
