/**
 * @file Config.cpp
 * @brief Bodies for src/utils/Config.hpp — compiled once into app_core: file
 *        load/parse, the dot-path walker and the global-instance lifecycle.
 *        The ${VAR} / ${VAR:-default} placeholder expansion lives in
 *        ConfigExpand.cpp (std+json only, shared with the fuzz harness); the
 *        typed template accessors (get / require / get_optional) stay in the
 *        header. Every contract is documented on the declarations there.
 */

#include "utils/Config.hpp"

#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#include "utils/ConfigExpand.hpp"

namespace Config {

AppConfig::AppConfig(const std::string& config_file) {
    config_path = config_file;
    load_from_file(config_file);
}

void AppConfig::load_from_file(const std::string& file_path) {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open config file: " + file_path);
    }

    try {
        file >> config_data;
    } catch (const json::parse_error& e) {
        throw std::runtime_error("Failed to parse config file: " + std::string(e.what()));
    }

    detail::substitute_env_placeholders(config_data);
}

const json* AppConfig::find_nested_node(const std::string& key) const {
    const json* current = &config_data;
    size_t start = 0;
    while (true) {
        const size_t pos = key.find('.', start);
        const std::string segment = (pos == std::string::npos) ? key.substr(start) : key.substr(start, pos - start);
        if (!current->is_object())
            return nullptr;
        const auto it = current->find(segment);
        if (it == current->end())
            return nullptr;
        current = &it.value();
        if (pos == std::string::npos)
            return current;
        start = pos + 1;
    }
}

namespace {

/// Global configuration instance. File-local on purpose: everything outside
/// this TU reaches it through initialize() / get() / is_initialized() /
/// shutdown() below (nothing else ever referenced it directly when it was an
/// inline header variable).
std::unique_ptr<AppConfig> global_config = nullptr;

}  // namespace

void initialize(const std::string& config_file) {
    if (global_config != nullptr) {
        throw std::runtime_error("Configuration already initialized");
    }
    global_config = std::make_unique<AppConfig>(config_file);
}

AppConfig& get() {
    if (global_config == nullptr) {
        throw std::runtime_error("Configuration not initialized");
    }
    return *global_config;
}

bool is_initialized() {
    return global_config != nullptr;
}

void shutdown() {
    global_config.reset();
}

}  // namespace Config
