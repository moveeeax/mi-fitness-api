/**
 * @file DataController.cpp
 * @brief Bodies for src/api/DataController.hpp — compiled once into app_core.
 */

#include "api/DataController.hpp"

#include <exception>

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "api/RequestUtils.hpp"
#include "utils/ErrorResponse.hpp"
#include "xiaomi/Regions.hpp"

namespace Api {

using json = nlohmann::json;

namespace {

constexpr long kDefaultLimit = 1000;
constexpr long kMaxLimit = 10000;

}  // namespace

bool DataController::parse_query(const HttpRequestPtr& req,
                                 Query& query,
                                 const std::function<void(const HttpResponsePtr&)>& callback) {
    query.from = req->getParameter("from");
    query.to = req->getParameter("to");
    if (query.from.empty() || query.to.empty()) {
        callback(ErrorResponse::bad_request("invalid_range", "from and to are required as YYYY-MM-DD"));
        return false;
    }
    try {
        // Смещение пояса на проверку формата не влияет.
        Xiaomi::range_to_timestamps(query.from, query.to, "cn");
    } catch (const Xiaomi::MiFitnessProtocolError&) {
        callback(ErrorResponse::bad_request("invalid_range",
                                            "from and to must be YYYY-MM-DD and from must not be after to"));
        return false;
    }
    query.limit = kDefaultLimit;
    query.offset = 0;
    const std::string limit = req->getParameter("limit");
    const std::string offset = req->getParameter("offset");
    try {
        if (!limit.empty()) {
            query.limit = std::stol(limit);
        }
        if (!offset.empty()) {
            query.offset = std::stol(offset);
        }
    } catch (const std::exception&) {
        callback(ErrorResponse::bad_request("invalid_pagination", "limit and offset must be integers"));
        return false;
    }
    if (query.limit < 1 || query.limit > kMaxLimit || query.offset < 0) {
        callback(
            ErrorResponse::bad_request("invalid_pagination", "limit must be 1..10000 and offset must not be negative"));
        return false;
    }
    return true;
}

void DataController::respond_page(const std::function<Repositories::HealthReadRepository::Page()>& read,
                                  const std::function<void(const HttpResponsePtr&)>& callback) {
    try {
        const auto page = read();
        callback(Response::ok(json{{"data", page.rows}, {"count", page.rows.size()}, {"total", page.total}}));
    } catch (const std::exception& e) {
        // База лежит: состояние инфраструктуры, не 500 без следа в логе.
        spdlog::warn("data read unavailable: {}", e.what());
        callback(ErrorResponse::service_unavailable("data_unavailable"));
    }
}

void DataController::dailyActivity(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    Query q;
    if (!parse_query(req, q, callback))
        return;
    respond_page([q] { return Repositories::HealthReadRepository().daily_activity(q.from, q.to, q.limit, q.offset); },
                 callback);
}

void DataController::sleep(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    Query q;
    if (!parse_query(req, q, callback))
        return;
    respond_page([q] { return Repositories::HealthReadRepository().sleep(q.from, q.to, q.limit, q.offset); }, callback);
}

void DataController::heartRate(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    Query q;
    if (!parse_query(req, q, callback))
        return;
    const std::string type = req->getParameter("type");
    if (!type.empty() && type != "passive" && type != "active" && type != "resting" && type != "manual") {
        callback(ErrorResponse::bad_request("invalid_type", "type must be passive, active, resting or manual"));
        return;
    }
    respond_page(
        [q, type] { return Repositories::HealthReadRepository().heart_rate(q.from, q.to, type, q.limit, q.offset); },
        callback);
}

void DataController::stress(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    Query q;
    if (!parse_query(req, q, callback))
        return;
    respond_page([q] { return Repositories::HealthReadRepository().stress(q.from, q.to, q.limit, q.offset); },
                 callback);
}

void DataController::spo2(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    Query q;
    if (!parse_query(req, q, callback))
        return;
    respond_page([q] { return Repositories::HealthReadRepository().spo2(q.from, q.to, q.limit, q.offset); }, callback);
}

void DataController::body(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    Query q;
    if (!parse_query(req, q, callback))
        return;
    respond_page([q] { return Repositories::HealthReadRepository().body(q.from, q.to, q.limit, q.offset); }, callback);
}

void DataController::workouts(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    Query q;
    if (!parse_query(req, q, callback))
        return;
    respond_page([q] { return Repositories::HealthReadRepository().workouts(q.from, q.to, q.limit, q.offset); },
                 callback);
}

void DataController::summary(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    Query q;
    if (!parse_query(req, q, callback))
        return;
    respond_page([q] { return Repositories::HealthReadRepository().summary(q.from, q.to, q.limit, q.offset); },
                 callback);
}

}  // namespace Api
