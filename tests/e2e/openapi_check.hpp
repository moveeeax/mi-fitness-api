/**
 * @file openapi_check.hpp
 * @brief Validate real e2e HTTP response bodies against docs/openapi.yaml.
 *
 * The route gates (check-openapi-drift.sh / check-routes-registered.sh)
 * compare only (method, path) tuples — nothing ever checked that the BODIES
 * the server actually sends match the schemas the spec promises. This header
 * closes that gap for the e2e bucket: after an existing request, call
 * expect_matches_schema(resp, "GET", "/api/v1/auth/me", 200) and the body is
 * validated against paths → responses → content → application/json → schema
 * from the spec.
 *
 * The spec is read from tests/e2e/openapi.gen.json — a committed JSON
 * conversion of docs/openapi.yaml produced by scripts/gen-openapi-json.sh
 * (no YAML parser is linked here, and adding one via vcpkg rebuilds the
 * dependency world). Freshness is enforced by expect_spec_json_fresh():
 * the JSON embeds an FNV-1a-64 hash of the raw YAML bytes and this file
 * re-hashes the YAML with the same function — a stale artifact fails the
 * suite with regeneration instructions instead of validating against an
 * old spec.
 *
 * Deliberately a SUBSET validator (nlohmann only, no JSON-Schema library):
 *   supported   — type (object/array/string/integer/number/boolean/null,
 *                 including ["string","null"] arrays), required, properties,
 *                 items, enum, nullable, additionalProperties (boolean),
 *                 $ref into "#/components/schemas/...".
 *   ignored     — annotations: format, description, title, default, examples.
 *   unsupported — everything else (oneOf/anyOf/allOf, pattern, minLength,
 *                 minimum, minProperties, ...): reported as an explicit
 *                 "[openapi-schema] SKIP" line, never a silent pass. Today's
 *                 response schemas use none of these; the log line is the
 *                 tripwire for the day one appears.
 */

#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

namespace OpenApiCheck {

using json = nlohmann::json;

// Both paths are relative to the repo root, which is the cwd of every e2e
// run (compose WORKDIR /app; `make coverage` runs from the checkout) — the
// same assumption the server boot makes for migrations/ and logs/.
inline constexpr const char* kSpecJsonPath = "tests/e2e/openapi.gen.json";
inline constexpr const char* kSpecYamlPath = "docs/openapi.yaml";

inline bool read_file(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

/// FNV-1a 64 — mirrored in scripts/gen-openapi-json.sh; keep in sync.
inline std::uint64_t fnv1a64(const std::string& bytes) {
    std::uint64_t h = 0xCBF29CE484222325ULL;
    for (unsigned char c : bytes) {
        h ^= c;
        h *= 0x100000001B3ULL;
    }
    return h;
}

inline std::string to_hex16(std::uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

/// The parsed spec, loaded once per process. json() (null) when unloadable —
/// every consumer turns that into a test failure with instructions.
inline const json& spec() {
    static const json s = [] {
        std::string raw;
        if (!read_file(kSpecJsonPath, raw))
            return json();
        auto parsed = json::parse(raw, /*cb=*/nullptr, /*allow_exceptions=*/false);
        return parsed.is_discarded() ? json() : parsed;
    }();
    return s;
}

/**
 * The staleness gate: tests/e2e/openapi.gen.json must have been generated
 * from the docs/openapi.yaml sitting in THIS tree. Call from a dedicated
 * test so drift fails loudly even when the sidecars are down.
 */
inline void expect_spec_json_fresh() {
    ASSERT_FALSE(spec().is_null()) << kSpecJsonPath << " is missing or not valid JSON — regenerate it:\n"
                                   << "    ./scripts/gen-openapi-json.sh";
    const json meta = spec().value("x-generated", json());
    ASSERT_TRUE(meta.is_object()) << kSpecJsonPath << " has no x-generated stamp — regenerate it:\n"
                                  << "    ./scripts/gen-openapi-json.sh";
    std::string yaml_raw;
    ASSERT_TRUE(read_file(kSpecYamlPath, yaml_raw))
        << "cannot read " << kSpecYamlPath << " — e2e must run from the repo root";
    EXPECT_EQ(meta.value("source_fnv1a64", ""), to_hex16(fnv1a64(yaml_raw)))
        << kSpecJsonPath << " is STALE: docs/openapi.yaml changed after it was generated.\n"
        << "Regenerate and commit it:\n"
        << "    ./scripts/gen-openapi-json.sh";
}

struct Result {
    std::vector<std::string> errors;  ///< schema violations — become test failures
    std::vector<std::string> skips;   ///< unsupported constructs — logged, never silent
};

inline std::string type_name(const json& v) {
    switch (v.type()) {
        case json::value_t::null:
            return "null";
        case json::value_t::boolean:
            return "boolean";
        case json::value_t::string:
            return "string";
        case json::value_t::array:
            return "array";
        case json::value_t::object:
            return "object";
        case json::value_t::number_float:
            return "number";
        default:
            return v.is_number_integer() || v.is_number_unsigned() ? "integer" : "unknown";
    }
}

inline bool matches_type(const std::string& t, const json& v) {
    if (t == "object")
        return v.is_object();
    if (t == "array")
        return v.is_array();
    if (t == "string")
        return v.is_string();
    if (t == "integer")
        return v.is_number_integer() || v.is_number_unsigned();
    if (t == "number")
        return v.is_number();
    if (t == "boolean")
        return v.is_boolean();
    if (t == "null")
        return v.is_null();
    return false;  // unknown type keyword — caller records a skip
}

inline std::string brief(const json& v) {
    std::string s = v.dump();
    return s.size() > 160 ? s.substr(0, 160) + "..." : s;
}

/// Recursive core. `where` is a JSON-pointer-ish location inside the instance.
inline void validate(const json& schema, const json& instance, const std::string& where, Result& out, int depth = 0) {
    if (depth > 32) {
        out.skips.push_back(where + ": recursion depth cap hit — not validated deeper");
        return;
    }
    if (!schema.is_object()) {
        out.skips.push_back(where + ": non-object schema — not validated");
        return;
    }

    // $ref — only the components/schemas form the spec uses.
    if (auto ref = schema.find("$ref"); ref != schema.end()) {
        const std::string target = ref->get<std::string>();
        constexpr const char* kPrefix = "#/components/schemas/";
        if (target.rfind(kPrefix, 0) != 0) {
            out.skips.push_back(where + ": unsupported $ref form '" + target + "'");
            return;
        }
        const json resolved = spec()
                                  .value("components", json::object())
                                  .value("schemas", json::object())
                                  .value(target.substr(std::string(kPrefix).size()), json());
        if (resolved.is_null()) {
            out.errors.push_back(where + ": $ref target '" + target + "' not found in components/schemas");
            return;
        }
        validate(resolved, instance, where, out, depth + 1);
        return;
    }

    // Loud skip for constraint keywords this subset does not implement.
    static const std::set<std::string> kHandled = {
        "type", "required", "properties", "items", "enum", "nullable", "additionalProperties"};
    static const std::set<std::string> kAnnotations = {
        "format", "description", "title", "default", "example", "examples", "deprecated"};
    for (auto it = schema.begin(); it != schema.end(); ++it) {
        if (kHandled.count(it.key()) || kAnnotations.count(it.key()))
            continue;
        out.skips.push_back(where + ": keyword '" + it.key() + "' not supported by this validator — not checked");
    }

    // nullable: true — OpenAPI 3.0 spelling, used alongside type.
    if (instance.is_null() && schema.value("nullable", false))
        return;

    // type — a single string, or an array of type names (3.1 spelling of
    // nullability: type: ['string', 'null']).
    if (auto t = schema.find("type"); t != schema.end()) {
        bool ok = false;
        std::string wanted;
        if (t->is_string()) {
            wanted = t->get<std::string>();
            ok = matches_type(wanted, instance);
        } else if (t->is_array()) {
            for (const auto& alt : *t) {
                wanted += (wanted.empty() ? "" : "|") + alt.get<std::string>();
                ok = ok || matches_type(alt.get<std::string>(), instance);
            }
        }
        if (!ok) {
            out.errors.push_back(where + ": expected type '" + wanted + "', got " + type_name(instance) + " (" +
                                 brief(instance) + ")");
            return;  // structural checks below would only cascade
        }
    }

    if (auto e = schema.find("enum"); e != schema.end()) {
        bool ok = false;
        for (const auto& allowed : *e)
            ok = ok || allowed == instance;
        if (!ok)
            out.errors.push_back(where + ": value " + brief(instance) + " not in enum " + e->dump());
    }

    if (instance.is_object()) {
        for (const auto& req : schema.value("required", json::array()))
            if (!instance.contains(req.get<std::string>()))
                out.errors.push_back(where + ": required property '" + req.get<std::string>() + "' is missing");
        const json props = schema.value("properties", json::object());
        for (auto it = props.begin(); it != props.end(); ++it)
            if (instance.contains(it.key()))
                validate(it.value(), instance.at(it.key()), where + "/" + it.key(), out, depth + 1);
        if (auto ap = schema.find("additionalProperties"); ap != schema.end()) {
            if (ap->is_boolean() && !ap->get<bool>()) {
                for (auto it = instance.begin(); it != instance.end(); ++it)
                    if (!props.contains(it.key()))
                        out.errors.push_back(where + ": property '" + it.key() +
                                             "' not allowed (additionalProperties: false)");
            } else if (ap->is_object()) {
                out.skips.push_back(where +
                                    ": schema-valued additionalProperties not supported — extra "
                                    "properties not checked");
            }
        }
    }

    if (instance.is_array()) {
        if (auto items = schema.find("items"); items != schema.end())
            for (std::size_t i = 0; i < instance.size(); ++i)
                validate(*items, instance[i], where + "/" + std::to_string(i), out, depth + 1);
    }
}

/**
 * Validate @p body against the response schema the spec declares for
 * (method, path template, status). Missing operation/status → failure (the
 * spec must document what the suite provokes); a response documented WITHOUT
 * an application/json schema → explicit SKIP line, nothing silently passes.
 */
inline void expect_matches_schema(const std::string& body,
                                  const std::string& method,
                                  const std::string& path,
                                  int status) {
    const std::string label = method + " " + path + " -> " + std::to_string(status);
    SCOPED_TRACE("openapi schema check: " + label);
    ASSERT_FALSE(spec().is_null()) << kSpecJsonPath << " missing/unparsable — run ./scripts/gen-openapi-json.sh";

    std::string m = method;
    for (auto& c : m)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const json op = spec().value("paths", json::object()).value(path, json::object()).value(m, json());
    ASSERT_TRUE(op.is_object()) << "spec has no operation for " << label
                                << " — the (method, path) gates should have caught this";
    const json rsp = op.value("responses", json::object()).value(std::to_string(status), json());
    ASSERT_TRUE(rsp.is_object()) << "spec does not document status " << status << " for " << method << " " << path
                                 << " — the server just sent it; document it in docs/openapi.yaml";

    const json schema =
        rsp.value("content", json::object()).value("application/json", json::object()).value("schema", json());
    if (schema.is_null()) {
        std::cout << "[openapi-schema] SKIP " << label
                  << ": no application/json schema declared — nothing to validate against\n";
        return;
    }

    const json instance = json::parse(body, nullptr, /*allow_exceptions=*/false);
    ASSERT_FALSE(instance.is_discarded()) << label << ": response body is not valid JSON: " << brief(json(body));

    Result res;
    validate(schema, instance, "#", res);
    for (const auto& s : res.skips)
        std::cout << "[openapi-schema] SKIP " << label << " " << s << "\n";
    if (!res.errors.empty()) {
        std::string all;
        for (const auto& e : res.errors)
            all += "  " + e + "\n";
        ADD_FAILURE() << "response body violates the OpenAPI schema for " << label << ":\n"
                      << all << "body: " << brief(instance);
    }
}

}  // namespace OpenApiCheck
