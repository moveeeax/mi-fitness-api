/**
 * @file Templates.cpp
 * @brief Bodies for src/email/Templates.hpp — compiled once into app_core:
 *        template lookup/read, HTML escaping and the inja renders. Every
 *        contract is documented on the declarations in the header; holding
 *        the <inja/inja.hpp> include here keeps inja out of every
 *        consumer's include graph.
 */

#include "email/Templates.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <inja/inja.hpp>
#include <nlohmann/json.hpp>

#include "utils/Config.hpp"

namespace Email::Templates {

std::string templates_dir() {
    if (Config::is_initialized()) {
        return Config::get().get<std::string>("mail.templates_dir", "MAIL_TEMPLATES_DIR", "templates/email");
    }
    // Env override must work without Config too (unit tests, tooling) —
    // mirrors the layered lookup where env always wins anyway.
    if (const char* env = std::getenv("MAIL_TEMPLATES_DIR"))
        return env;
    return "templates/email";
}

namespace {

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.good())
        throw std::runtime_error("template not found: " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

std::string escape_html(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&#39;";
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}

namespace {

/**
 * @brief Recursively HTML-escape every string leaf of a render context.
 * @details Whole-context rather than per-key so a new template variable is
 *          safe by default — the failure mode of an opt-in list is a silent
 *          injection hole.
 */
void escape_html_values(json& node) {
    if (node.is_string()) {
        node = escape_html(node.get_ref<const std::string&>());
    } else if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it)
            escape_html_values(it.value());
    } else if (node.is_array()) {
        for (auto& v : node)
            escape_html_values(v);
    }
}

}  // namespace

std::string render(const std::string& name, const std::string& ext, const json& ctx) {
    const auto path = std::filesystem::path(templates_dir()) / (name + "." + ext);
    const std::string tpl = read_file(path.string());
    inja::Environment env;
    if (ext == "html") {
        json safe = ctx;
        escape_html_values(safe);
        return env.render(tpl, safe);
    }
    return env.render(tpl, ctx);
}

Pair render_pair(const std::string& name, const json& ctx) {
    Pair p;
    p.text = render(name, "txt", ctx);
    p.html = render(name, "html", ctx);
    return p;
}

json default_context() {
    json ctx = json::object();
    if (Config::is_initialized()) {
        ctx["app_name"] = Config::get().get<std::string>("app.name", "APP_NAME", "App");
        ctx["base_url"] = Config::get().get<std::string>("app.base_url", "APP_BASE_URL", "http://localhost:8080");
    } else {
        ctx["app_name"] = "App";
        ctx["base_url"] = "http://localhost:8080";
    }
    return ctx;
}

}  // namespace Email::Templates
