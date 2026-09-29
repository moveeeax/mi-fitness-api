/**
 * @file ConfigExpand.hpp
 * @brief ${VAR} / ${VAR:-default} env-placeholder expansion — the pure
 *        parsing half of config loading.
 * @details Free functions (they never touch AppConfig state; they were
 *          private statics on the class before the fuzzing work) so the
 *          fuzz_config_expand libFuzzer harness in tests/fuzz can compile
 *          ConfigExpand.cpp directly — this header pulls only <string> and
 *          nlohmann/json, not spdlog (which Config.hpp needs for its get<T>
 *          template and which would otherwise ride along into the fuzz
 *          build). AppConfig::load_from_file is the production caller.
 */

#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace Config::detail {

/**
 * @brief Expand ${VAR} and ${VAR:-default} placeholders in a single string.
 * @details Simple POSIX-shell-style substitution. Unmatched placeholders
 *          are replaced with empty string (or their default clause).
 */
std::string expand_string(const std::string& s);

/**
 * @brief Recursively walk JSON and expand placeholders in every string value.
 */
void substitute_env_placeholders(nlohmann::json& node);

}  // namespace Config::detail
