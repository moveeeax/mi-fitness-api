# tests/fuzz — libFuzzer harnesses for the byte-facing parsers.
#
# Design decision (documented in docs/TESTING.md, "Fuzzing"): the fuzzers do
# NOT link app_core. app_core drags the whole vcpkg dependency world (Drogon,
# pqxx, OTel, ...) which is built with GCC in CI — libFuzzer requires Clang,
# and rebuilding ~29 vcpkg packages under a second toolchain would blow any
# nightly budget. Instead each harness compiles the specific src/ TU that
# carries the parser under test; those TUs are kept std-only (or std+nlohmann
# for the config expander) exactly so this works:
#
#   fuzz_traceparent   <- src/observability/Trace.cpp      (std only)
#   fuzz_decimal_cents <- src/billing/PayPalParse.cpp      (std only)
#   fuzz_path_match    <- src/utils/Strings.cpp            (std only;
#                         normalize_path_for_metrics is header-only)
#   fuzz_config_expand <- src/utils/ConfigExpand.cpp       (std + nlohmann)
#
# nlohmann/json is the one third-party dependency and is header-only — the
# nightly job takes it from the distro package (nlohmann-json3-dev), no vcpkg.
#
# Two ways to configure:
#   * standalone (what .github/workflows/fuzz-nightly.yml does — no vcpkg
#     toolchain file, minutes not hours):
#       cmake -S tests/fuzz -B build-fuzz -DCMAKE_CXX_COMPILER=clang++
#       cmake --build build-fuzz -j
#   * embedded, from the top-level project: -DENABLE_FUZZERS=ON (requires the
#     full vcpkg toolchain to already be configured under Clang).
#
# Either way a non-Clang compiler is a hard configure error below.

cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(mi_fitness_api_fuzz CXX)
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "libFuzzer needs Clang; the compiler here is '${CMAKE_CXX_COMPILER_ID}'. "
        "Configure a dedicated fuzz build dir instead of the normal (GCC) one:\n"
        "  cmake -S tests/fuzz -B build-fuzz -DCMAKE_CXX_COMPILER=clang++\n"
        "  cmake --build build-fuzz -j")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(FUZZ_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# fuzzer = libFuzzer driver + coverage feedback; address+undefined make the
# "don't crash, no UB" oracle actually bite. No -fsanitize-recover: the first
# fault aborts and libFuzzer writes the reproducer input.
set(FUZZ_SAN_FLAGS -fsanitize=fuzzer,address,undefined)

function(add_fuzz_target name)
    add_executable(${name} ${name}.cpp ${ARGN})
    target_include_directories(${name} PRIVATE ${FUZZ_REPO_ROOT}/src)
    target_compile_options(${name} PRIVATE
        ${FUZZ_SAN_FLAGS}
        -fno-sanitize-recover=all
        -fno-omit-frame-pointer
        -g
        -O1)
    target_link_options(${name} PRIVATE ${FUZZ_SAN_FLAGS})
endfunction()

add_fuzz_target(fuzz_traceparent   ${FUZZ_REPO_ROOT}/src/observability/Trace.cpp)
add_fuzz_target(fuzz_decimal_cents ${FUZZ_REPO_ROOT}/src/billing/PayPalParse.cpp)
add_fuzz_target(fuzz_path_match    ${FUZZ_REPO_ROOT}/src/utils/Strings.cpp)
add_fuzz_target(fuzz_config_expand ${FUZZ_REPO_ROOT}/src/utils/ConfigExpand.cpp)

# nlohmann/json for the config expander: reuse the project's target when
# embedded; standalone, take the distro/homebrew package (config mode), or
# fall back to a bare include dir.
if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json CONFIG QUIET)
endif()
if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(fuzz_config_expand PRIVATE nlohmann_json::nlohmann_json)
else()
    find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp REQUIRED)
    target_include_directories(fuzz_config_expand PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
endif()
