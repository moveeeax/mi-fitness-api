/**
 * @file ErrorResponse.cpp
 * @brief Bodies for src/utils/ErrorResponse.hpp — compiled once into
 *        app_core. Every contract (and the one stable error shape) is
 *        documented on the declarations in the header.
 */

#include "utils/ErrorResponse.hpp"

#include <utility>

namespace ErrorResponse {

drogon::HttpResponsePtr make(Error e) {
    json body = {{"error", e.code}, {"status", static_cast<int>(e.status)}};
    if (!e.message.empty())
        body["message"] = std::move(e.message);
    if (e.extra.is_object())
        body.update(e.extra);
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(e.status);
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    resp->setBody(body.dump());
    return resp;
}

drogon::HttpResponsePtr bad_request(std::string code, std::string message, json extra) {
    return make({drogon::k400BadRequest, std::move(code), std::move(message), std::move(extra)});
}

drogon::HttpResponsePtr unauthorized(std::string code, std::string message) {
    return make({drogon::k401Unauthorized, std::move(code), std::move(message), json::object()});
}

drogon::HttpResponsePtr forbidden(std::string code, std::string message) {
    return make({drogon::k403Forbidden, std::move(code), std::move(message), json::object()});
}

drogon::HttpResponsePtr not_found(std::string what) {
    return make({drogon::k404NotFound, "not_found", what + " not found", json::object()});
}

drogon::HttpResponsePtr conflict(std::string code, std::string message) {
    return make({drogon::k409Conflict, std::move(code), std::move(message), json::object()});
}

drogon::HttpResponsePtr payload_too_large(std::string code) {
    return make({drogon::k413RequestEntityTooLarge, std::move(code), "", json::object()});
}

drogon::HttpResponsePtr unsupported_media_type(std::string code, std::string message) {
    return make({drogon::k415UnsupportedMediaType, std::move(code), std::move(message), json::object()});
}

drogon::HttpResponsePtr unprocessable(std::string code, std::string message) {
    return make({drogon::k422UnprocessableEntity, std::move(code), std::move(message), json::object()});
}

drogon::HttpResponsePtr too_many_requests(int retry_after_sec) {
    return make({drogon::k429TooManyRequests, "rate_limited", "", json{{"retry_after_sec", retry_after_sec}}});
}

drogon::HttpResponsePtr internal_error(std::string code, std::string message) {
    return make({drogon::k500InternalServerError, std::move(code), std::move(message), json::object()});
}

drogon::HttpResponsePtr service_unavailable(std::string code, std::string message) {
    return make({drogon::k503ServiceUnavailable, std::move(code), std::move(message), json::object()});
}

}  // namespace ErrorResponse

namespace Response {

drogon::HttpResponsePtr ok(const json& body) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setBody(body.dump());
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    resp->setStatusCode(drogon::k200OK);
    return resp;
}

drogon::HttpResponsePtr created(const json& body) {
    auto resp = ok(body);
    resp->setStatusCode(drogon::k201Created);
    return resp;
}

drogon::HttpResponsePtr paginated(const json& data, long total, int limit, int offset) {
    return ok({{"data", data}, {"total", total}, {"limit", limit}, {"offset", offset}});
}

drogon::HttpResponsePtr list(const json& data) {
    return ok({{"data", data}});
}

}  // namespace Response
