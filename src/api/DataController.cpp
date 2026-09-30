/**
 * @file DataController.cpp
 * @brief Bodies for src/api/DataController.hpp — compiled once into app_core.
 */

#include "api/DataController.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <vector>

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "api/RequestUtils.hpp"
#include "utils/Config.hpp"
#include "utils/ErrorResponse.hpp"
#include "xiaomi/Normalize.hpp"
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
    // Окна сна и resting-пульса строятся от локальной полуночи в поясе
    // региона: date у активности локальная (то же правило, что у границ
    // диапазона синка).
    std::string region;
    if (Config::is_initialized()) {
        region = Config::get().get<std::string>("xiaomi.region", "MI_FITNESS_REGION", "");
    }
    const long long offset = (region.empty() || region == "cn") ? 8 * 3600 : 0;
    respond_page(
        [q, offset] { return Repositories::HealthReadRepository().summary(q.from, q.to, q.limit, q.offset, offset); },
        callback);
}

void DataController::abnormalHeartBeat(const HttpRequestPtr& req,
                                       std::function<void(const HttpResponsePtr&)>&& callback) {
    Query q;
    if (!parse_query(req, q, callback))
        return;
    respond_page(
        [q] { return Repositories::HealthReadRepository().abnormal_heart_beat(q.from, q.to, q.limit, q.offset); },
        callback);
}

void DataController::coverage(const HttpRequestPtr& /*req*/, std::function<void(const HttpResponsePtr&)>&& callback) {
    try {
        callback(Response::ok(json{{"data", Repositories::HealthReadRepository().coverage()}}));
    } catch (const std::exception& e) {
        spdlog::warn("coverage unavailable: {}", e.what());
        callback(ErrorResponse::service_unavailable("data_unavailable"));
    }
}

void DataController::exportData(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    static const std::vector<std::string> kTypes = {"daily_activity",
                                                    "sleep",
                                                    "heart_rate",
                                                    "stress",
                                                    "spo2",
                                                    "body_measurements",
                                                    "workouts",
                                                    "abnormal_heart_beat"};
    Query q;
    if (!parse_query(req, q, callback))
        return;
    const std::string format = req->getParameter("format").empty() ? "json" : req->getParameter("format");
    const std::string type = req->getParameter("type");
    if (format != "json" && format != "csv") {
        callback(ErrorResponse::bad_request("invalid_format", "format must be json or csv"));
        return;
    }
    if (!type.empty() && std::find(kTypes.begin(), kTypes.end(), type) == kTypes.end()) {
        callback(ErrorResponse::bad_request("unknown_data_type", "type must be one of the exported datasets"));
        return;
    }
    // Без потолка ширины json_agg собирает годы данных одним значением в
    // памяти пода (обзор фазы 3, Important 8).
    const auto range = Xiaomi::range_to_timestamps(q.from, q.to, "cn");
    if (range.second - range.first > 366LL * 86400) {
        callback(ErrorResponse::bad_request("range_too_wide", "export covers at most 366 days per request"));
        return;
    }
    if (format == "csv" && type.empty()) {
        // CSV это плоская таблица одного типа; все типы разом это JSON.
        callback(ErrorResponse::bad_request("csv_needs_type", "csv export takes exactly one type"));
        return;
    }
    try {
        Repositories::HealthReadRepository repo;
        if (format == "json") {
            json records = json::object();
            if (type.empty()) {
                for (const auto& t : kTypes) {
                    records[t] = repo.export_rows(t, q.from, q.to);
                }
            } else {
                records[type] = repo.export_rows(type, q.from, q.to);
            }
            // Конверт как у Python-моста (export.py, schema_version 1.0):
            // потребители выгрузки не переучиваются.
            const auto now =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count();
            callback(Response::ok(
                json{{"schema_version", "1.0"},
                     {"source", "mi-fitness-api"},
                     {"generated_at", Xiaomi::detail::iso_with_offset(now, 0)},
                     {"filters",
                      {{"dataset", type.empty() ? json() : json(type)}, {"start_date", q.from}, {"end_date", q.to}}},
                     {"records", records}}));
            return;
        }
        const auto rows = repo.export_rows(type, q.from, q.to);
        auto resp = HttpResponse::newHttpResponse();
        resp->setContentTypeString("text/csv; charset=utf-8");
        resp->setBody(to_csv(rows));
        callback(resp);
    } catch (const std::exception& e) {
        spdlog::warn("export unavailable: {}", e.what());
        callback(ErrorResponse::service_unavailable("data_unavailable"));
    }
}

std::string DataController::to_csv(const nlohmann::json& rows) {
    if (!rows.is_array() || rows.empty()) {
        return "";
    }
    // Строковое значение, начинающееся (после пробелов) с = + - @ TAB CR,
    // получает апостроф: открытая в таблице выгрузка не должна исполнять
    // формулы. Правила _escape_csv_value моста; числа не трогаются, иначе
    // отрицательные величины превращаются в текст.
    const auto cell = [](const json& v) {
        std::string s;
        bool escapable = false;
        if (v.is_null()) {
            s = "";
        } else if (v.is_string()) {
            s = v.get<std::string>();
            escapable = true;
        } else {
            s = v.dump();
        }
        if (escapable) {
            const auto lead = s.find_first_not_of(' ');
            if (lead != std::string::npos) {
                const char c = s[lead];
                if (c == '=' || c == '+' || c == '-' || c == '@' || c == '\t' || c == '\r') {
                    s.insert(s.begin(), '\'');
                }
            }
        }
        if (s.find_first_of(",\"\n\r") != std::string::npos) {
            std::string quoted = "\"";
            for (const char c : s) {
                if (c == '\"') {
                    quoted += "\"\"";
                } else {
                    quoted += c;
                }
            }
            quoted += "\"";
            return quoted;
        }
        return s;
    };
    std::string out;
    bool first = true;
    for (const auto& [key, value] : rows[0].items()) {
        (void)value;
        if (!first) {
            out += ',';
        }
        out += key;
        first = false;
    }
    out += '\n';
    for (const auto& row : rows) {
        first = true;
        for (const auto& [key, value] : row.items()) {
            (void)key;
            if (!first) {
                out += ',';
            }
            out += cell(value);
            first = false;
        }
        out += '\n';
    }
    return out;
}

}  // namespace Api
