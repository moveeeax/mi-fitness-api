/**
 * @file HandlerSupport.hpp
 * @brief Shared controller-side helpers: map repository exceptions to the
 *        canonical HTTP error responses in one place.
 * @details Every mutating handler used to repeat the same try/catch ladder
 *          (DuplicateEmail->409, *NotFound->404, RoleInUse->409, std::exception
 *          ->500). That's ~13 copies that drift in their codes/messages and
 *          that a new handler can forget. with_repo_errors() centralizes the
 *          mapping so the error contract is defined once.
 *
 *          The repository layer still owns the BOUNDARY (it throws typed
 *          exceptions, it does not know about HTTP); this helper is the api-
 *          side translation of those types — the natural counterpart to
 *          Repositories::detail::translate_sql (SQLSTATE -> exception).
 *
 *          It catches the GENERIC bases (Repositories::NotFoundError /
 *          ConflictError, defined in RepoErrors.hpp), NOT the concrete domain
 *          exceptions — so this header does not depend on UserRepository /
 *          RoleRepository, and a forked domain's own NotFoundError/ConflictError
 *          subclasses map automatically with no edit here.
 */

#pragma once

#include <exception>
#include <functional>
#include <utility>

#include <drogon/HttpResponse.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "repositories/RepoErrors.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

/**
 * @brief Collect a range of json-convertible items into a JSON array — the
 *        `json::array()` + push_back loop every list handler repeats.
 */
template <class Range>
nlohmann::json to_json_array(const Range& items) {
    nlohmann::json data = nlohmann::json::array();
    for (const auto& item : items)
        data.push_back(item);
    return data;
}

/**
 * @brief Run @p fn, translating any repository exception into the canonical
 *        HTTP error response via @p cb. @p op is a short label for the 500
 *        log line ("admin createUser"). Returns true if @p fn completed
 *        without throwing — handlers that must keep running after the guarded
 *        block (e.g. to send a success response) can branch on it.
 *
 * The concrete domain exceptions carry their own code/resource and derive
 * from these bases, so the same stable machine codes the tests assert hold:
 *   DuplicateEmail -> 409 email_taken | UserNotFound -> 404 user
 *   DuplicateRole  -> 409 role_exists | RoleNotFound -> 404 role
 *   RoleInUse      -> 409 role_in_use | anything else -> 500
 *   Repositories::ValidationError subclasses -> 400 (malformed input at the
 *   SQL boundary, e.g. a non-UUID id)
 */
template <typename Fn>
inline bool with_repo_errors(const std::function<void(const drogon::HttpResponsePtr&)>& cb, const char* op, Fn&& fn) {
    try {
        fn();
        return true;
    } catch (const Repositories::ValidationError& e) {
        cb(ErrorResponse::bad_request(e.code(), e.message()));
    } catch (const Repositories::ConflictError& e) {
        cb(ErrorResponse::conflict(e.code(), e.message()));
    } catch (const Repositories::NotFoundError& e) {
        cb(ErrorResponse::not_found(e.resource()));
    } catch (const std::exception& e) {
        spdlog::error("{} failed: {}", op, e.what());
        cb(ErrorResponse::internal_error());
    }
    return false;
}

/**
 * @brief with_repo_errors + an after-response side effect (email dispatch,
 *        enqueue, webhook fire). @p after_fn runs only if @p fn completed
 *        without throwing, and runs OUTSIDE the catch ladder above — so a
 *        throwing side effect can log, but can never be translated into a
 *        SECOND cb(...) invocation after the response was already sent.
 *
 * Incident this encodes (downstream billing fork, site cd8279c): a
 * receipt-email dispatch placed INSIDE the guarded lambda threw after
 * callback() had already fired the success response; with_repo_errors' own
 * catch then mapped the throw and invoked callback() a second time. The fix
 * was to stash the result in a std::optional captured by reference and
 * dispatch after with_repo_errors returned. This overload is that discipline
 * as an API:
 *
 *   std::optional<Result> result;
 *   with_repo_errors(callback, "op",
 *       [&] { ...; callback(Response::ok(...)); result = r; },
 *       [&] { if (result) dispatch_email(*result); });
 *
 * Note @p after_fn runs whenever @p fn completed (even if @p fn responded
 * with its own early-return error) — gate domain conditions inside after_fn,
 * e.g. on the stashed optional as above. Exceptions from after_fn are logged
 * and swallowed: the response is already on the wire, there is nothing
 * correct left to send.
 */
template <typename Fn, typename AfterFn>
inline bool with_repo_errors(const std::function<void(const drogon::HttpResponsePtr&)>& cb,
                             const char* op,
                             Fn&& fn,
                             AfterFn&& after_fn) {
    const bool completed = with_repo_errors(cb, op, std::forward<Fn>(fn));
    if (!completed)
        return false;
    // Deliberately OUTSIDE any cb-invoking try/catch — see the doc comment.
    try {
        after_fn();
    } catch (const std::exception& e) {
        spdlog::error("{}: after-response side effect failed: {}", op, e.what());
    } catch (...) {
        spdlog::error("{}: after-response side effect failed with a non-std exception", op);
    }
    return true;
}

}  // namespace Api
