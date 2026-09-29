/**
 * @file SqlErrors.hpp
 * @brief Shared SQLSTATE→domain-exception translation for repositories.
 *
 * Every write that can trip a constraint used to repeat the same
 * `catch (const pqxx::sql_error& e) { if (std::string(e.sqlstate()) == "…")
 * throw DomainError{}; throw; }`. translate_sql() captures that shape once:
 * run the body, and on a pqxx::sql_error hand the SQLSTATE (as a
 * non-allocating string_view) to a translator that throws the appropriate
 * typed exception — falling through to rethrow the original otherwise.
 */

#pragma once

#include <pqxx/pqxx>
#include <string_view>

namespace Repositories::detail {

/**
 * @brief Run @p fn; translate a pqxx::sql_error via @p translate.
 * @param fn         The repository operation (usually a Database::execute_*).
 * @param translate  Invoked with the SQLSTATE on sql_error. It may throw a
 *                   domain exception; if it returns, the original error is
 *                   rethrown unchanged.
 */
template <typename Fn, typename Translator>
auto translate_sql(Fn&& fn, Translator&& translate) -> decltype(fn()) {
    try {
        return fn();
    } catch (const pqxx::sql_error& e) {
        translate(std::string_view(e.sqlstate()));
        throw;
    }
}

/**
 * @brief Translator factory for the ubiquitous single-SQLSTATE case: returns
 *        a translator that throws E when the SQLSTATE equals @p state and
 *        falls through (original error rethrown by translate_sql) otherwise.
 *        Call with a string literal — the lambda stores the view.
 */
template <typename E>
auto throw_on(std::string_view state) {
    return [state](std::string_view ss) {
        if (ss == state)
            throw E{};
    };
}

}  // namespace Repositories::detail
