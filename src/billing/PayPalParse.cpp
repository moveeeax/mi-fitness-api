/**
 * @file PayPalParse.cpp
 * @brief The pure byte-facing parser helpers of Billing::detail — split out
 *        of PayPalClient.cpp so this TU is std-only (no curl/spdlog/json):
 *        the fuzz_decimal_cents libFuzzer harness in tests/fuzz compiles it
 *        directly, with no vcpkg dependencies. Contracts are documented on
 *        the declarations in PayPalClient.hpp; keep network-touching or
 *        json-touching code (describe_error_body, the curl callback) in
 *        PayPalClient.cpp.
 */

#include <cctype>
#include <cstddef>
#include <sstream>
#include <stdexcept>

#include "billing/PayPalClient.hpp"

namespace Billing {

namespace detail {

std::int64_t parse_decimal_to_cents(const std::string& s) {
    if (s.empty())
        throw std::runtime_error("paypal: empty amount string");
    if (s.front() == '+' || s.front() == '-')
        throw std::runtime_error("paypal: signed amount not accepted: '" + s + "'");

    const auto dot = s.find('.');
    const std::string int_part = (dot == std::string::npos) ? s : s.substr(0, dot);
    std::string frac_part = (dot == std::string::npos) ? std::string() : s.substr(dot + 1);

    if (int_part.empty())
        throw std::runtime_error("paypal: malformed amount '" + s + "'");
    if (int_part.size() > 15)
        throw std::runtime_error("paypal: amount '" + s + "' out of range");
    for (char c : int_part)
        if (!std::isdigit(static_cast<unsigned char>(c)))
            throw std::runtime_error("paypal: malformed amount '" + s + "'");

    if (dot != std::string::npos) {
        if (frac_part.empty())
            throw std::runtime_error("paypal: malformed amount '" + s + "' (trailing '.')");
        if (frac_part.size() > 2)
            throw std::runtime_error("paypal: amount '" + s + "' has more than 2 fractional digits");
        for (char c : frac_part)
            if (!std::isdigit(static_cast<unsigned char>(c)))
                throw std::runtime_error("paypal: malformed amount '" + s + "'");
    }
    while (frac_part.size() < 2)
        frac_part.push_back('0');

    const std::int64_t whole = std::stoll(int_part);
    const std::int64_t frac = std::stoll(frac_part);
    return whole * 100 + frac;
}

std::string cents_to_decimal_string(std::int64_t cents) {
    if (cents < 0)
        throw std::runtime_error("paypal: negative amount_cents");
    const std::int64_t whole = cents / 100;
    const std::int64_t frac = cents % 100;
    std::ostringstream oss;
    oss << whole << '.' << (frac < 10 ? "0" : "") << frac;
    return oss.str();
}

std::string find_header_ci(const std::map<std::string, std::string>& headers, const std::string& name) {
    for (const auto& kv : headers) {
        if (kv.first.size() != name.size())
            continue;
        bool match = true;
        for (std::size_t i = 0; i < name.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(kv.first[i])) !=
                std::tolower(static_cast<unsigned char>(name[i]))) {
                match = false;
                break;
            }
        }
        if (match)
            return kv.second;
    }
    return {};
}

std::string url_encode_segment(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~')
            out.push_back(static_cast<char>(c));
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

}  // namespace detail

}  // namespace Billing
