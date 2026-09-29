/**
 * @file JobsController.cpp
 * @brief Bodies for src/api/JobsController.hpp — compiled once into
 *        app_core. Contract and route-ordering notes are documented on the
 *        declarations in the header.
 */

#include "api/JobsController.hpp"

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/RequestUtils.hpp"
#include "api/Validation.hpp"
#include "jobs/Jobs.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

void JobsController::listJobs(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    API_REQUIRE_ADMIN(req, callback);
    API_REQUIRE_JOBS_READY(callback);
    try {
        auto type_param = req->getParameter("type");
        const auto pp = parse_page_params(req, /*default_limit=*/20, /*max_limit=*/200);

        auto page = Jobs::get().list_paged(type_param, pp.limit, pp.offset);
        json jobs_json = json::array();
        for (const auto& job : page.jobs) {
            jobs_json.push_back(job.to_json());
        }
        callback(Response::paginated(jobs_json, page.total, pp.limit, pp.offset));
    } catch (const std::exception& e) {
        spdlog::error("Error in GET /api/v1/jobs: {}", e.what());
        callback(ErrorResponse::internal_error());
    }
}

void JobsController::submitJob(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    API_REQUIRE_ADMIN(req, callback);
    API_REQUIRE_JOBS_READY(callback);
    try {
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Validation::Errors errs;
        Validation::require(errs, body, "type");
        Validation::string_length(errs, body, "type", 1, 255);
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        auto type = body["type"].get<std::string>();
        auto payload = body.value("payload", json::object());
        int max_retries = body.value("max_retries", -1);

        auto job = Jobs::get().submit(type, payload, max_retries);
        callback(Response::created({{"data", job.to_json()}, {"message", "Job submitted"}}));
    } catch (const std::exception& e) {
        spdlog::error("Error in POST /api/v1/jobs: {}", e.what());
        callback(ErrorResponse::internal_error());
    }
}

void JobsController::getJobStatus(const HttpRequestPtr& req,
                                  std::function<void(const HttpResponsePtr&)>&& callback,
                                  const std::string& id) {
    API_REQUIRE_ADMIN(req, callback);
    API_REQUIRE_JOBS_READY(callback);
    try {
        if (!require_valid_uuid(id, callback))
            return;
        auto job = Jobs::get().get_status(id);
        if (!job) {
            callback(ErrorResponse::not_found("job"));
            return;
        }
        callback(Response::ok({{"data", job->to_json()}}));
    } catch (const std::exception& e) {
        spdlog::error("Error in GET /api/v1/jobs/{}: {}", id, e.what());
        callback(ErrorResponse::internal_error());
    }
}

void JobsController::listDlq(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
    API_REQUIRE_ADMIN(req, callback);
    API_REQUIRE_JOBS_READY(callback);
    try {
        auto type_param = req->getParameter("type");
        int limit = clamp_int(req->getParameter("limit"), 100, 1, 500);

        auto jobs = Jobs::get().list_dlq(type_param, limit);
        json jobs_json = json::array();
        for (const auto& j : jobs)
            jobs_json.push_back(j.to_json());
        callback(Response::ok({{"data", jobs_json}, {"count", jobs_json.size()}, {"depth", Jobs::get().dlq_depth()}}));
    } catch (const std::exception& e) {
        spdlog::error("Error in GET /api/v1/jobs/dlq: {}", e.what());
        callback(ErrorResponse::internal_error());
    }
}

void JobsController::requeueDlq(const HttpRequestPtr& req,
                                std::function<void(const HttpResponsePtr&)>&& callback,
                                const std::string& id) {
    API_REQUIRE_ADMIN(req, callback);
    API_REQUIRE_JOBS_READY(callback);
    try {
        if (!require_valid_uuid(id, callback))
            return;
        if (!Jobs::get().requeue_from_dlq(id)) {
            callback(ErrorResponse::not_found("dlq_job"));
            return;
        }
        callback(Response::ok({{"message", "Job requeued from DLQ"}, {"id", id}}));
    } catch (const std::exception& e) {
        spdlog::error("Error in POST /api/v1/jobs/dlq/{}/requeue: {}", id, e.what());
        callback(ErrorResponse::internal_error());
    }
}

void JobsController::cancelJob(const HttpRequestPtr& req,
                               std::function<void(const HttpResponsePtr&)>&& callback,
                               const std::string& id) {
    API_REQUIRE_ADMIN(req, callback);
    API_REQUIRE_JOBS_READY(callback);
    try {
        if (!require_valid_uuid(id, callback))
            return;
        if (!Jobs::get().cancel(id)) {
            callback(ErrorResponse::not_found("cancellable_job"));
            return;
        }
        callback(Response::ok({{"message", "Job cancelled"}}));
    } catch (const std::exception& e) {
        spdlog::error("Error in DELETE /api/v1/jobs/{}: {}", id, e.what());
        callback(ErrorResponse::internal_error());
    }
}

}  // namespace Api
