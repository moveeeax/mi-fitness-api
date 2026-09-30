/**
 * @file XiaomiController.hpp
 * @brief Проверка связи с облаком Mi Fitness: логин, один ключ данных, счётчик.
 *
 * Маршрут отдаёт количество записей и замаскированный идентификатор аккаунта,
 * но никогда сами записи: это проверка транспорта и подписи, а не выгрузка.
 * Единственная точка, где порт крипты подтверждается настоящим облаком.
 *
 * Запрос к облаку блокирующий и занимает секунды. Для служебного маршрута,
 * который дёргают руками при выкатке и отладке, это приемлемо.
 */

#pragma once

#include <drogon/HttpController.h>
#include <drogon/drogon.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

// Controllers must NOT include api/Api.hpp (it includes the controllers — that
// would cycle). Pull only the small shared helpers.
#include "api/RequestUtils.hpp"
#include "repositories/CredentialsRepository.hpp"
#include "repositories/SyncRunRepository.hpp"
#include "utils/ErrorResponse.hpp"
#include "xiaomi/CloudClient.hpp"
#include "xiaomi/DataKeys.hpp"
#include "xiaomi/Regions.hpp"
#include "xiaomi/Service.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class XiaomiController : public HttpController<XiaomiController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(XiaomiController::probe, "/api/v1/xiaomi/probe", Get);
    METHOD_LIST_END

    void probe(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        // Синк и probe не живут одновременно: оба логинятся, а каждый логин
        // ротирует passToken. Живой запуск в журнале — probe отказывается
        // до похода в облако (Global Constraint фазы 3).
        try {
            if (Repositories::SyncRunRepository().any_running()) {
                callback(ErrorResponse::conflict("sync_in_progress", "a sync run is in progress, retry later"));
                return;
            }
        } catch (const std::exception&) {
            // Недоступная база не должна прятать probe: он сам упрётся в неё
            // ниже и ответит честнее.
        }

        const std::string key = req->getParameter("key");
        const std::string from = req->getParameter("from");
        const std::string to = req->getParameter("to");

        // Ошибки клиента отсеиваются до учётных данных и до сети: опечатка в
        // ключе или дате это 400, а не 503.
        if (!Xiaomi::is_known_data_key(key)) {
            callback(ErrorResponse::bad_request("unknown_key", "key must be one of the Mi Fitness data keys"));
            return;
        }
        try {
            // Смещение пояса на валидность формата не влияет, регион тут не нужен.
            Xiaomi::range_to_timestamps(from, to, "cn");
        } catch (const Xiaomi::MiFitnessProtocolError&) {
            callback(ErrorResponse::bad_request("invalid_range",
                                                "from and to must be YYYY-MM-DD and from must not be after to"));
            return;
        }

        const std::string token_key = Xiaomi::Service::token_key_b64();
        if (token_key.empty()) {
            callback(ErrorResponse::service_unavailable("not_configured", "MI_FITNESS_TOKEN_KEY is not set"));
            return;
        }

        try {
            Repositories::CredentialsRepository repository(token_key);
            const auto credentials = repository.load();
            if (!credentials.has_value()) {
                callback(ErrorResponse::service_unavailable("not_configured", "Xiaomi credentials are not seeded yet"));
                return;
            }

            Xiaomi::CloudClient client(
                Xiaomi::Service::transport(), *credentials, [&repository](const Xiaomi::Credentials& rotated) {
                    repository.store(rotated);
                });
            client.login();
            const auto items = client.fetch_key(key, from, to, std::nullopt);

            callback(Response::ok(json{{"data",
                                        {{"account", Xiaomi::mask_account_id(client.credentials().user_id)},
                                         {"region", client.credentials().region},
                                         {"key", key},
                                         {"records", items.size()}}}}));
        } catch (const Xiaomi::MiFitnessAuthError& e) {
            // Лечится только свежим токеном, поэтому код отличим от прочих
            // отказов. Текст исключения наружу не идёт, но в лог обязан:
            // иначе не отличить мёртвый токен от сломанной ступени логина.
            // Тексты MiFitness*Error по построению не содержат значений.
            spdlog::warn("xiaomi probe auth failure: {}", e.what());
            callback(ErrorResponse::service_unavailable("upstream_auth", "Xiaomi refused the stored credentials"));
        } catch (const Xiaomi::MiFitnessProtocolError& e) {
            spdlog::warn("xiaomi probe protocol failure: {}", e.what());
            callback(ErrorResponse::service_unavailable("upstream_protocol",
                                                        "Xiaomi response did not match the expected format"));
        }
    }
};

}  // namespace Api
