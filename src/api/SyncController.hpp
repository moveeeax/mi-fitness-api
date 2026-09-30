/**
 * @file SyncController.hpp
 * @brief Маршруты синка: постановка запуска в очередь и чтение журнала.
 *
 * POST создаёт строку журнала со статусом queued и кладёт задание xiaomi_sync
 * в очередь: сам синк идёт в воркере, у API нет причин держать соединение
 * минуты. Журнал читается по run_id.
 */

#pragma once

#include <string>
#include <vector>

#include <drogon/HttpController.h>
#include <drogon/drogon.h>

#include <nlohmann/json.hpp>

// Controllers must NOT include api/Api.hpp (it includes the controllers — that
// would cycle). Pull only the small shared helpers.
#include "api/RequestUtils.hpp"
#include "jobs/Jobs.hpp"
#include "repositories/SyncRunRepository.hpp"
#include "sync/SyncService.hpp"
#include "utils/ErrorResponse.hpp"
#include "xiaomi/Regions.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class SyncController : public HttpController<SyncController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(SyncController::enqueue, "/api/v1/sync", Post);
    ADD_METHOD_TO(SyncController::status, "/api/v1/sync/{id}", Get);
    METHOD_LIST_END

    void enqueue(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        json body = json::parse(std::string(req->body()), nullptr, /*allow_exceptions=*/false);
        if (body.is_discarded() || !body.is_object() || !body.contains("from") || !body.contains("to") ||
            !body["from"].is_string() || !body["to"].is_string()) {
            callback(ErrorResponse::bad_request("invalid_body", "body must carry from and to as YYYY-MM-DD"));
            return;
        }
        const std::string from = body["from"].get<std::string>();
        const std::string to = body["to"].get<std::string>();
        try {
            // Смещение пояса на валидность формата не влияет.
            Xiaomi::range_to_timestamps(from, to, "cn");
        } catch (const Xiaomi::MiFitnessProtocolError&) {
            callback(ErrorResponse::bad_request("invalid_range",
                                                "from and to must be YYYY-MM-DD and from must not be after to"));
            return;
        }
        std::vector<std::string> data_types = Sync::kAllDataTypes;
        if (body.contains("data_types")) {
            if (!body["data_types"].is_array() || body["data_types"].empty()) {
                callback(ErrorResponse::bad_request("invalid_data_types", "data_types must be a non-empty array"));
                return;
            }
            data_types.clear();
            for (const auto& item : body["data_types"]) {
                if (!item.is_string() ||
                    std::find(Sync::kAllDataTypes.begin(), Sync::kAllDataTypes.end(), item.get<std::string>()) ==
                        Sync::kAllDataTypes.end()) {
                    callback(ErrorResponse::bad_request("unknown_data_type", "data_types must be Mi Fitness types"));
                    return;
                }
                data_types.push_back(item.get<std::string>());
            }
        }

        Repositories::SyncRunRepository runs;
        const long run_id = runs.create(from, to, data_types);
        Jobs::get().submit(Jobs::XiaomiSync::kJobType,
                           json{{"run_id", run_id}, {"from", from}, {"to", to}, {"data_types", data_types}});
        auto resp = Response::ok(json{{"data", {{"run_id", run_id}, {"status", "queued"}}}});
        resp->setStatusCode(k202Accepted);
        callback(resp);
    }

    void status(const HttpRequestPtr& /*req*/,
                std::function<void(const HttpResponsePtr&)>&& callback,
                const std::string& id) {
        long run_id = 0;
        try {
            std::size_t consumed = 0;
            run_id = std::stol(id, &consumed);
            if (consumed != id.size() || run_id <= 0) {
                throw std::invalid_argument("trailing garbage");
            }
        } catch (const std::exception&) {
            callback(ErrorResponse::bad_request("invalid_id", "run id must be a positive integer"));
            return;
        }
        const auto row = Repositories::SyncRunRepository().get(run_id);
        if (!row.has_value()) {
            callback(ErrorResponse::not_found("run_not_found"));
            return;
        }
        callback(Response::ok(json{{"data", *row}}));
    }
};

}  // namespace Api
