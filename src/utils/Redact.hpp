/**
 * @file Redact.hpp
 * @brief Mask credentials in a resolved config tree before it is printed.
 *
 * `--dump-config` resolves ${VAR} placeholders in place, so the raw tree
 * holds the real DB/Redis/SMTP/S3/JWT secrets — dumping it verbatim turns a
 * debugging aid into a credential printer (found live downstream:
 * tarassov.me round-3 L8). Header-only per ADR 0003 so the masking is unit
 * -testable without linking the binary entry point.
 */

#pragma once

#include <cctype>
#include <string>

#include <nlohmann/json.hpp>

namespace Utils::Redact {

/// True when a config KEY names a credential. Substring match on the
/// lower-cased key — the usual password|secret|token|key convention.
inline bool key_is_secret(const std::string& key) {
    std::string k;
    k.reserve(key.size());
    for (char c : key)
        k += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return k.find("password") != std::string::npos || k.find("secret") != std::string::npos ||
           k.find("token") != std::string::npos || k.find("key") != std::string::npos;
}

/// scheme://user:pass@host/db → scheme://user:***@host/db. The DB/Redis DSNs
/// carry the password inline under an innocuous key ("primary", "url"), so
/// the key-name rule above never sees them.
inline std::string mask_dsn_userinfo(const std::string& value) {
    const auto scheme = value.find("://");
    if (scheme == std::string::npos)
        return value;
    const auto at = value.find('@', scheme + 3);
    if (at == std::string::npos)
        return value;
    const auto colon = value.find(':', scheme + 3);
    if (colon == std::string::npos || colon > at)
        return value;  // userinfo without a password
    return value.substr(0, colon + 1) + "***" + value.substr(at);
}

/// Redact every credential in a resolved config tree, in place. An EMPTY
/// value stays empty: "is this set?" is the whole point of --dump-config, so
/// the masked form keeps the length instead of hiding the set-vs-unset
/// signal.
inline void mask_secrets(nlohmann::json& node) {
    if (node.is_array()) {
        for (auto& element : node)
            mask_secrets(element);
        return;
    }
    if (!node.is_object())
        return;
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (!it->is_string()) {
            mask_secrets(*it);
            continue;
        }
        const auto& value = it->get_ref<const std::string&>();
        if (value.empty())
            continue;
        if (key_is_secret(it.key()))
            *it = "***set (" + std::to_string(value.size()) + " chars)";
        else
            *it = mask_dsn_userinfo(value);
    }
}

}  // namespace Utils::Redact
