/**
 * @file fuzz_config_expand.cpp
 * @brief libFuzzer harness for Config::detail::expand_string /
 *        substitute_env_placeholders — the ${VAR} / ${VAR:-default}
 *        expansion runs over every string in the config file, which in a
 *        fork may embed operator-supplied text. Oracle: never crash/UB on
 *        arbitrary bytes, both on a bare string and recursively through a
 *        JSON document. The environment is cleared and re-seeded in
 *        LLVMFuzzerInitialize so runs are deterministic and the fuzzer can
 *        actually hit the set / empty / unset branches. Compiled with
 *        src/utils/ConfigExpand.cpp (std + nlohmann/json only — no spdlog).
 */

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <nlohmann/json.hpp>

#include "utils/ConfigExpand.hpp"

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/) {
#ifdef __linux__
    clearenv();  // determinism: no host env leaks into expansion results
#endif
    setenv("FUZZ_SET_VAR", "fuzz-value", 1);
    setenv("FUZZ_EMPTY_VAR", "", 1);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string s(reinterpret_cast<const char*>(data), size);

    (void)Config::detail::expand_string(s);

    // The production path: expansion walks a parsed JSON document
    // recursively (objects, arrays, mixed leaf types).
    nlohmann::json j;
    j["top"] = s;
    j["nested"]["inner"] = s;
    j["arr"] = nlohmann::json::array({s, 42, nullptr, true});
    Config::detail::substitute_env_placeholders(j);
    return 0;
}
