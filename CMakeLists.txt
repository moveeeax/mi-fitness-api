cmake_minimum_required(VERSION 3.21)
project(mi_fitness_api VERSION 1.8.2 LANGUAGES CXX)

# Release builds stamp the git tag in instead of the baked value above:
# docker/Dockerfile declares ARG APP_VERSION and forwards it as
# -DPROJECT_VERSION_OVERRIDE, and .github/workflows/release.yml passes the tag.
# The baked value is the fallback for every non-tag build, so it must equal the
# newest CHANGELOG release heading — scripts/check-version-sync.sh gates that
# in CI (it sat three releases stale at 1.4.0 with nothing checking).
if(PROJECT_VERSION_OVERRIDE)
    set(PROJECT_VERSION "${PROJECT_VERSION_OVERRIDE}")
    message(STATUS "Application version overridden by build arg: ${PROJECT_VERSION}")
endif()

# Set C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags. Base set is intentionally strict — extra warnings catch
# real bugs (shadowing, unsafe casts, narrowing, format mismatches) without
# adding false-positive noise on a modern GCC/Clang. Toggle ENABLE_WERROR
# (default OFF) for CI gates; locally we don't want a warning to block work.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

add_compile_options(
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Woverloaded-virtual
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
)

option(ENABLE_WERROR "Treat warnings as errors (intended for CI)" OFF)
if(ENABLE_WERROR)
    # Two GCC middle-end diagnostics stay warnings instead of errors: at -O3
    # they fire from inlined third-party internals (nlohmann::json via the
    # PCH, std::variant inside pqxx::params) and escape -isystem suppression
    # because the optimizer attributes them to the system-header location —
    # a known false-positive class we can't fix in third-party code.
    # -Warray-bounds joins the list for the same reason: GCC 13 at -O3 emits a
    # bogus std::string-SSO bounds error (libstdc++ operator+ inlining, cf. GCC
    # PR105651) that comes and goes with unrelated inlining-context changes.
    add_compile_options(-Werror -Wno-error=null-dereference -Wno-error=maybe-uninitialized
                        -Wno-error=array-bounds)
endif()

# Sanitizer support — when ENABLE_SANITIZERS=ON, build with ASan+UBSan
# (Debug build recommended). Intended for CI and local bug-hunting; do not
# ship to production. Enforces strict mode (no suppressions, abort on fault).
option(ENABLE_SANITIZERS "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(ENABLE_SANITIZERS)
    message(STATUS "Sanitizers enabled: address + undefined")
    add_compile_options(
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
        -O1
        -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# ThreadSanitizer — mutually exclusive with ASan/UBSan (they can't share a
# process). Used by the CI `tsan` job over the unit-test bucket.
option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)
if(ENABLE_TSAN)
    if(ENABLE_SANITIZERS)
        message(FATAL_ERROR "ENABLE_TSAN and ENABLE_SANITIZERS are mutually exclusive")
    endif()
    message(STATUS "Sanitizer enabled: thread")
    add_compile_options(
        -fsanitize=thread
        -fno-omit-frame-pointer
        -O1
        -g)
    add_link_options(-fsanitize=thread)
endif()

# Coverage instrumentation — enables gcov-compatible counters across the
# whole project so `make coverage` (gcovr/lcov) can produce an HTML report.
# Implies Debug-ish build (no -O3, with -g). Use the `coverage` preset.
option(ENABLE_COVERAGE "Build with gcov coverage instrumentation" OFF)
if(ENABLE_COVERAGE)
    message(STATUS "Coverage enabled (--coverage)")
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

# Find required packages
find_package(Drogon CONFIG REQUIRED)
find_package(libpqxx CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)
pkg_check_modules(REDIS_PLUS_PLUS REQUIRED IMPORTED_TARGET redis++)
pkg_check_modules(UUID REQUIRED IMPORTED_TARGET uuid)
find_package(RdKafka CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(prometheus-cpp CONFIG REQUIRED)
find_package(opentelemetry-cpp CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)  # Used directly for HMAC-SHA256 (Auth) and SHA-256 (Idempotency)
# libsodium comes from the OS (apt: libsodium-dev / brew: libsodium).
# vcpkg's libsodium port fails to build on arm64-linux, so we route
# through pkg-config which works on every platform we care about.
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
# libcurl from the OS for SMTP in src/email/Mailer.hpp. We use the
# upstream FindCURL module (not CONFIG) so this works against the
# apt-installed package — no vcpkg curl port needed.
find_package(CURL REQUIRED)
# inja: single-header Jinja-equivalent for C++. We use the pre-built
# single-header file instead of FetchContent on the full repo — inja
# v3.4.0 ships unconditional install(TARGETS nlohmann_json) which
# conflicts with vcpkg's nlohmann_json target. Pulling just the single
# header avoids that whole CMake mess.
#
# VENDORED, not downloaded. This used to be a file(DOWNLOAD ...) from
# raw.githubusercontent.com at CONFIGURE time, which made every clean build
# depend on GitHub being reachable AND not rate-limiting us. When GitHub
# answers 429 the sanitizer/tsan jobs cannot configure at all — a build
# failing for a reason that has nothing to do with the code (downstream bump:
# cyber-accountant f708e6d). Vendoring the one header removes the dependency.
#
# The hash below is still asserted, now against the checked-in copy, so an
# accidental edit to a vendored third-party file fails the configure step
# instead of shipping quietly. It is the SAME hash the download used to
# verify, so this is provably the identical file (inja v3.4.0, MIT, license
# text kept in the header — see third_party/inja-include/README.md).
set(INJA_VERSION v3.4.0)
set(INJA_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/inja-include)
set(INJA_EXPECTED_SHA256 8022ce5a782e911598accc42311ac44a0b9ccd9ad5995f439c828fb534974543)
file(SHA256 ${INJA_INCLUDE_DIR}/inja/inja.hpp INJA_ACTUAL_SHA256)
if(NOT INJA_ACTUAL_SHA256 STREQUAL INJA_EXPECTED_SHA256)
    message(FATAL_ERROR
        "third_party/inja-include/inja/inja.hpp does not match the expected inja ${INJA_VERSION} hash.\n"
        "  expected: ${INJA_EXPECTED_SHA256}\n"
        "  actual:   ${INJA_ACTUAL_SHA256}\n"
        "A vendored third-party header must not be edited in place.")
endif()
add_library(pantor_inja INTERFACE)
add_library(pantor::inja ALIAS pantor_inja)
target_include_directories(pantor_inja INTERFACE ${INJA_INCLUDE_DIR})
# inja's single header includes <nlohmann/json.hpp>, so we wire the
# nlohmann_json dependency through this interface target — no
# explicit nlohmann_json link in COMMON_LIBS would be enough either.
target_link_libraries(pantor_inja INTERFACE nlohmann_json::nlohmann_json)

# Single source of the version: project(VERSION ...) above renders into
# generated/version.hpp; Core::version() includes it. Bump the version in
# project() only.
configure_file(src/version.hpp.in ${CMAKE_BINARY_DIR}/generated/version.hpp @ONLY)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)

# Shared link libraries (used by main app, worker, and tests)
set(COMMON_LIBS
    Drogon::Drogon
    libpqxx::pqxx
    PkgConfig::HIREDIS
    PkgConfig::REDIS_PLUS_PLUS
    PkgConfig::UUID
    RdKafka::rdkafka
    RdKafka::rdkafka++
    spdlog::spdlog
    prometheus-cpp::core
    prometheus-cpp::pull
    opentelemetry-cpp::api
    opentelemetry-cpp::sdk
    opentelemetry-cpp::trace
    opentelemetry-cpp::metrics
    opentelemetry-cpp::ostream_span_exporter
    opentelemetry-cpp::otlp_http_exporter
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
    PkgConfig::SODIUM
    pantor::inja     # alias from inja's CMakeLists (real target name: `inja`)
    CURL::libcurl
    Threads::Threads
)

# ── app_core ─────────────────────────────────────────────────────────────────
# The single named seam every binary (server, worker, tests) builds against:
# it carries the shared third-party deps + the src/generated include dirs, so a
# new dependency is added in ONE place and every target picks it up.
#
# STATIC since the Phase 2 modularity work (spec:
# docs/superpowers/specs/2026-08-22-modularity-design.md; ADR 0003's 2026-08-22
# amendment): de-inlined module bodies live in src/**/*.cpp and compile ONCE
# here instead of once per binary. The GLOB mirrors the test-source GLOBs
# below — a new src/<dir>/Foo.cpp is picked up automatically (CONFIGURE_DEPENDS
# re-runs the glob per build); only the two binary entry points are excluded,
# they stay per-executable. Modules are de-inlined by measured weight
# (scripts/bench-incremental.sh), one module per PR — billing first.
file(GLOB_RECURSE APP_CORE_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
list(REMOVE_ITEM APP_CORE_SOURCES
    ${CMAKE_SOURCE_DIR}/src/main.cpp
    ${CMAKE_SOURCE_DIR}/src/worker_main.cpp
)
add_library(app_core STATIC ${APP_CORE_SOURCES})
# PUBLIC (was INTERFACE): app_core's own .cpp files compile against these
# deps/include dirs too, and every linking binary still inherits them.
target_link_libraries(app_core PUBLIC ${COMMON_LIBS})
target_include_directories(app_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)

# Precompiled headers: the framework headers below dominate compile time
# (every TU re-parses Drogon + pqxx + redis++ + json). app_core owns the
# canonical PCH; the app/worker binaries REUSE_FROM it (a REUSE_FROM source
# must own its PCH outright, so both binaries point at app_core directly —
# never at each other). The test binaries need <gtest/gtest.h> on top, so
# tests_unit builds its own PCH and integration/e2e reuse THAT one.
set(HEAVY_PCH
    <drogon/drogon.h>
    <drogon/HttpController.h>
    <pqxx/pqxx>
    <sw/redis++/redis++.h>
    <nlohmann/json.hpp>
    <spdlog/spdlog.h>
    # inja (~6k lines, heavy templates) is pulled into every controller TU via
    # AccountEmails -> Templates for the inline-send fallback. Precompile it.
    <inja/inja.hpp>
)
target_precompile_headers(app_core PRIVATE ${HEAVY_PCH})

# Create main executable
add_executable(${PROJECT_NAME}
    src/main.cpp
)
target_link_libraries(${PROJECT_NAME} PRIVATE app_core)
target_precompile_headers(${PROJECT_NAME} REUSE_FROM app_core)

# Create worker executable
add_executable(${PROJECT_NAME}_worker
    src/worker_main.cpp
)
target_link_libraries(${PROJECT_NAME}_worker PRIVATE app_core)
target_precompile_headers(${PROJECT_NAME}_worker REUSE_FROM app_core)

# Enable testing
enable_testing()
include(GoogleTest)

# Test source classification by DIRECTORY — the only source of truth.
# Adding a new suite that needs Postgres/Redis: drop it in tests/integration/.
# Adding a pure unit suite: drop it in tests/unit/.
# No name-based filter list to keep in sync anywhere.
#
# tests/test_main.cpp — shared GlobalTestEnvironment (SetUp/TearDown only),
#   compiled into both binaries below.
# tests/unit/       — no external services; fast, always runnable.
# tests/integration/ + tests/api/ — need Postgres + Redis.
# tests/e2e/        — separate binary: owns drogon::app() run()/quit() cycle.
file(GLOB UNIT_SOURCES       CONFIGURE_DEPENDS "tests/unit/*.cpp")
file(GLOB INTEGRATION_SOURCES CONFIGURE_DEPENDS "tests/integration/*.cpp" "tests/api/*.cpp")
file(GLOB E2E_SOURCES        CONFIGURE_DEPENDS "tests/e2e/*.cpp")
set(TEST_MAIN_SOURCE "tests/test_main.cpp")

# ── Unit test binary ────────────────────────────────────────────────────────
# No Postgres/Redis needed. `ctest -L unit` or run the binary directly.
if(UNIT_SOURCES)
    add_executable(${PROJECT_NAME}_tests_unit
        ${TEST_MAIN_SOURCE}
        ${UNIT_SOURCES}
    )
    target_link_libraries(${PROJECT_NAME}_tests_unit PRIVATE
        GTest::gtest
        GTest::gmock
        app_core
    )
    target_include_directories(${PROJECT_NAME}_tests_unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests
    )
    target_precompile_headers(${PROJECT_NAME}_tests_unit PRIVATE ${HEAVY_PCH} <gtest/gtest.h>)

    # Per-test discovery for `ctest -R SomeSuite.someCase`.
    gtest_discover_tests(${PROJECT_NAME}_tests_unit
        PROPERTIES LABELS "unit"
    )
    # Aggregate entry: `ctest -L unit` runs the whole binary in one shot.
    add_test(NAME unit_bucket
        COMMAND ${PROJECT_NAME}_tests_unit --gtest_color=yes)
    set_tests_properties(unit_bucket PROPERTIES LABELS "unit")
endif()

# ── Integration test binary ─────────────────────────────────────────────────
# Needs Postgres + Redis. `ctest -L integration` or run the binary directly.
if(INTEGRATION_SOURCES)
    add_executable(${PROJECT_NAME}_tests_integration
        ${TEST_MAIN_SOURCE}
        ${INTEGRATION_SOURCES}
    )
    target_link_libraries(${PROJECT_NAME}_tests_integration PRIVATE
        GTest::gtest
        GTest::gmock
        app_core
    )
    target_include_directories(${PROJECT_NAME}_tests_integration PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests
    )
    target_precompile_headers(${PROJECT_NAME}_tests_integration REUSE_FROM ${PROJECT_NAME}_tests_unit)

    gtest_discover_tests(${PROJECT_NAME}_tests_integration
        PROPERTIES LABELS "integration"
    )
    add_test(NAME integration_bucket
        COMMAND ${PROJECT_NAME}_tests_integration --gtest_color=yes)
    set_tests_properties(integration_bucket PROPERTIES LABELS "integration")
endif()

# ── End-to-end binary ───────────────────────────────────────────────────────
# Real Drogon server + real HTTP client. Separate from the main test binaries
# because drogon::app() is once-per-process.
if(E2E_SOURCES)
    add_executable(${PROJECT_NAME}_e2e ${E2E_SOURCES})
    target_link_libraries(${PROJECT_NAME}_e2e PRIVATE
        GTest::gtest
        app_core
    )
    target_include_directories(${PROJECT_NAME}_e2e PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests
    )
    target_precompile_headers(${PROJECT_NAME}_e2e REUSE_FROM ${PROJECT_NAME}_tests_unit)
    add_test(NAME e2e_bucket COMMAND ${PROJECT_NAME}_e2e --gtest_color=yes)
    set_tests_properties(e2e_bucket PROPERTIES LABELS "e2e")
endif()

# ── libFuzzer harnesses (opt-in; Clang only) ────────────────────────────────
# OFF by default and completely inert when OFF — the normal (GCC) build is
# untouched. tests/fuzz/CMakeLists.txt hard-errors on a non-Clang compiler,
# and its normal entry point is a STANDALONE configure into build-fuzz that
# needs no vcpkg at all (the harnesses compile the parser TUs directly
# instead of linking app_core) — see the comment there and docs/TESTING.md,
# "Fuzzing". This option exists for a vcpkg-under-Clang tree that wants the
# fuzzers inside the main build graph.
option(ENABLE_FUZZERS "Build the libFuzzer harnesses in tests/fuzz (requires Clang)" OFF)
if(ENABLE_FUZZERS)
    add_subdirectory(tests/fuzz)
endif()

# Installation
install(TARGETS ${PROJECT_NAME} ${PROJECT_NAME}_worker
    RUNTIME DESTINATION bin
)

# Copy config files
install(DIRECTORY config/
    DESTINATION etc/${PROJECT_NAME}
    FILES_MATCHING PATTERN "*.json"
)
