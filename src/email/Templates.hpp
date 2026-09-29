/**
 * @file Templates.hpp
 * @brief Render email templates from disk via inja.
 *
 * Declarations only — the bodies live in Templates.cpp (compiled once
 * into app_core; ADR 0003 as amended 2026-08-22). That's also where the
 * <inja/inja.hpp> include lives now, keeping inja out of every
 * consumer's include graph.
 *
 * flask-base parity: app/email.py renders Jinja2 templates with the
 * Flask app context. We use inja (a Jinja-subset engine for C++) and
 * pass a plain nlohmann::json as context.
 *
 * Convention: every template ships as a .txt + .html pair under
 * templates/email/<name>.{txt,html}. Mailer expects both bodies, so
 * render_pair() returns them together.
 *
 * Templates are not cached — files are small and Mailer::send is the
 * dominant cost (network roundtrip). When this becomes a hotspot,
 * cache by mtime in inja::Environment.
 *
 * inja has NO autoescaping. Context values are user-controlled (display
 * names, email addresses), so render() HTML-escapes every string leaf of
 * the context on the .html path — see escape_html_values() in
 * Templates.cpp. The .txt path stays verbatim: escaping there would show
 * "&amp;" to the reader.
 */

#pragma once

#include <string>

#include <nlohmann/json_fwd.hpp>

namespace Email::Templates {

using json = nlohmann::json;

/**
 * @brief Configurable directory root. Defaults to "templates/email" relative
 *        to the working directory; override via mail.templates_dir /
 *        MAIL_TEMPLATES_DIR for non-standard layouts.
 */
std::string templates_dir();

/**
 * @brief Escape the five markup-significant characters.
 * @details Covers text nodes and both quoting styles, so one pass is safe
 *          for `<p>{{ x }}</p>` and `href="{{ x }}"` alike.
 */
std::string escape_html(const std::string& s);

/**
 * @brief Render one variant (txt or html) of @p name with @p ctx.
 * @details On the html variant every string in @p ctx is HTML-escaped first
 *          (inja does not autoescape); txt renders the context verbatim.
 * @throws std::runtime_error if the file is missing — caller decides
 *         whether that's fatal.
 */
std::string render(const std::string& name, const std::string& ext, const json& ctx);

struct Pair {
    std::string text;
    std::string html;
};

/**
 * @brief Render both .txt and .html variants in one go. The two share the
 *        same context — same variables, same values, except that the html
 *        variant sees them HTML-escaped (see render()).
 */
Pair render_pair(const std::string& name, const json& ctx);

/**
 * @brief Common context fields injected into every template render.
 *        flask-base used Flask's `current_app.config['APP_NAME']` etc.;
 *        we pass them explicitly so a unit test can render templates
 *        without booting Config.
 */
json default_context();

}  // namespace Email::Templates
