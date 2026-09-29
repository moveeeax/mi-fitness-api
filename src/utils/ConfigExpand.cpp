/**
 * @file ConfigExpand.cpp
 * @brief Bodies for src/utils/ConfigExpand.hpp — compiled once into
 *        app_core, and compiled directly into the fuzz_config_expand
 *        libFuzzer harness (tests/fuzz). Keep this TU free of anything
 *        beyond std + nlohmann/json (see the header note).
 */

#include "utils/ConfigExpand.hpp"

#include <cstdlib>

namespace Config::detail {

std::string expand_string(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (i + 1 < s.size() && s[i] == '$' && s[i + 1] == '{') {
            size_t end = s.find('}', i + 2);
            if (end == std::string::npos) {
                out.append(s, i, std::string::npos);
                break;
            }
            std::string expr = s.substr(i + 2, end - i - 2);
            std::string var_name;
            std::string default_value;
            bool has_default = false;
            auto sep = expr.find(":-");
            if (sep != std::string::npos) {
                var_name = expr.substr(0, sep);
                default_value = expr.substr(sep + 2);
                has_default = true;
            } else {
                var_name = expr;
            }
            const char* env_value = var_name.empty() ? nullptr : std::getenv(var_name.c_str());
            if (env_value != nullptr) {
                out.append(env_value);
            } else if (has_default) {
                out.append(default_value);
            }
            // else: leave the placeholder unexpanded? No — drop it silently
            // (matches POSIX shell behavior for unset-without-default).
            i = end + 1;
        } else {
            out.push_back(s[i++]);
        }
    }
    return out;
}

void substitute_env_placeholders(nlohmann::json& node) {
    if (node.is_string()) {
        const auto& raw = node.get_ref<const std::string&>();
        if (raw.find("${") != std::string::npos) {
            node = expand_string(raw);
        }
    } else if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it) {
            substitute_env_placeholders(it.value());
        }
    } else if (node.is_array()) {
        for (auto& v : node)
            substitute_env_placeholders(v);
    }
}

}  // namespace Config::detail
