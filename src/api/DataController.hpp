/**
 * @file DataController.hpp
 * @brief Чтение данных здоровья: восемь GET под /api/v1/data.
 *
 * Общие правила: диапазон дат from/to обязателен (YYYY-MM-DD, from не позже
 * to), пагинация limit/offset с полным total в ответе, сортировка по
 * времени, времена наружу в ISO UTC. Авторизация как у остальных маршрутов:
 * слой middleware, в api.public_paths этих путей нет.
 *
 * Declarations only — the handler bodies live in DataController.cpp
 * (compiled once into app_core; ADR 0003 as amended 2026-08-22). The route
 * macros (ADD_METHOD_TO) must stay in this header: Drogon's METHOD_LIST
 * registration is part of the class definition, and
 * scripts/check-routes-registered.sh greps the src/api headers for them.
 */

#pragma once

#include <functional>
#include <string>

#include <drogon/HttpController.h>

#include <nlohmann/json_fwd.hpp>

#include "repositories/HealthReadRepository.hpp"

namespace Api {

using namespace drogon;

class DataController : public HttpController<DataController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(DataController::dailyActivity, "/api/v1/data/daily-activity", Get);
    ADD_METHOD_TO(DataController::sleep, "/api/v1/data/sleep", Get);
    ADD_METHOD_TO(DataController::heartRate, "/api/v1/data/heart-rate", Get);
    ADD_METHOD_TO(DataController::stress, "/api/v1/data/stress", Get);
    ADD_METHOD_TO(DataController::spo2, "/api/v1/data/spo2", Get);
    ADD_METHOD_TO(DataController::body, "/api/v1/data/body", Get);
    ADD_METHOD_TO(DataController::workouts, "/api/v1/data/workouts", Get);
    ADD_METHOD_TO(DataController::summary, "/api/v1/data/summary", Get);
    ADD_METHOD_TO(DataController::coverage, "/api/v1/data/coverage", Get);
    ADD_METHOD_TO(DataController::exportData, "/api/v1/data/export", Get);
    METHOD_LIST_END

    void dailyActivity(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
    void sleep(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
    void heartRate(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
    void stress(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
    void spo2(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
    void body(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
    void workouts(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
    void summary(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
    void coverage(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
    void exportData(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

private:
    /// Разобранные параметры запроса чтения.
    struct Query {
        std::string from;
        std::string to;
        long limit = 1000;
        long offset = 0;
    };

    /**
     * @brief Разобрать from/to/limit/offset. На ошибке отвечает 400 через
     *        @p callback и возвращает false.
     */
    static bool parse_query(const HttpRequestPtr& req,
                            Query& query,
                            const std::function<void(const HttpResponsePtr&)>& callback);

    /// JSON-строки в CSV: заголовок из ключей первой строки, значения с
    /// ведущими = + - @ экранируются апострофом.
    static std::string to_csv(const nlohmann::json& rows);

    /// Общий хвост: выполнить чтение и завернуть страницу в ответ.
    static void respond_page(const std::function<Repositories::HealthReadRepository::Page()>& read,
                             const std::function<void(const HttpResponsePtr&)>& callback);
};

}  // namespace Api
