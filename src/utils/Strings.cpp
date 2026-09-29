/**
 * @file Strings.cpp
 * @brief Bodies for src/utils/Strings.hpp — compiled once into app_core.
 *        Every contract is documented on the declarations in the header.
 */

#include "utils/Strings.hpp"

#include <cstdlib>
#include <sstream>
#include <utility>

namespace Utils::Strings {

bool path_is_public(const std::unordered_set<std::string>& public_paths, const std::string& path) {
    if (public_paths.count(path) > 0)
        return true;
    for (const auto& p : public_paths) {
        if (!p.empty() && p.back() == '*') {
            const std::string_view prefix(p.data(), p.size() - 1);
            if (path.size() >= prefix.size() && path.compare(0, prefix.size(), prefix) == 0)
                return true;
        }
    }
    return false;
}

bool flag_true(std::string_view value) {
    return value == "true" || value == "1" || value == "yes";
}

bool env_flag_true(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && flag_true(value);
}

std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0)
            out += sep;
        out += v[i];
    }
    return out;
}

std::vector<std::string> split_csv_vec(const std::string& csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string piece;
    while (std::getline(ss, piece, ',')) {
        // Trim surrounding whitespace so "a, b" yields {"a","b"} not {"a"," b"}
        // — public-path / whitelist / CORS configs are routinely written with
        // spaces after commas.
        const size_t a = piece.find_first_not_of(" \t\r\n");
        if (a == std::string::npos)
            continue;  // all-whitespace / empty
        const size_t b = piece.find_last_not_of(" \t\r\n");
        out.push_back(piece.substr(a, b - a + 1));
    }
    return out;
}

std::unordered_set<std::string> split_csv_set(const std::string& csv) {
    auto v = split_csv_vec(csv);
    return {v.begin(), v.end()};
}

std::unordered_set<std::string> merge_csv_sets(const std::string& base_csv, const std::string& extra_csv) {
    auto out = split_csv_set(base_csv);
    for (auto& p : split_csv_vec(extra_csv))
        out.insert(std::move(p));
    return out;
}

std::string mask_email(const std::string& email) {
    if (email.empty())
        return email;
    const auto at = email.find('@');
    const std::string local = (at == std::string::npos) ? email : email.substr(0, at);
    const std::string domain = (at == std::string::npos) ? std::string() : email.substr(at);  // includes '@'
    const std::string masked = (local.size() > 1) ? (std::string(1, local[0]) + "***") : "***";
    return masked + domain;
}

}  // namespace Utils::Strings
