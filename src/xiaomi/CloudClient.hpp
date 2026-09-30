/**
 * @file CloudClient.hpp
 * @brief Клиент закрытого облака Mi Fitness: логин и подписанные запросы.
 *
 * Знает HTTP и подпись, не знает ни домена, ни базы. Нормализация записей и
 * хранение живут выше по стеку.
 */

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "xiaomi/Credentials.hpp"
#include "xiaomi/Errors.hpp"
#include "xiaomi/HttpTransport.hpp"

namespace Xiaomi {

/**
 * @brief Разрешён ли адрес редиректа после входа.
 *
 * Правила повторяют апстрим: только https, только домены xiaomi.com и mi.com
 * вместе с поддоменами, без учётных данных в адресе, без нестандартного порта,
 * без пробельных и управляющих символов. Всё остальное это канал утечки токена,
 * потому что на цель редиректа уходят куки сессии.
 */
bool is_allowed_login_redirect(std::string_view url);

class CloudClient {
public:
    /**
     * @param transport шов до сети, живёт дольше клиента
     * @param credentials стартовые учётные данные
     * @param on_rotate вызывается, когда Xiaomi выдал новый passToken. Исключение
     *        из обработчика не роняет логин: токен уже выдан, и сессия должна
     *        продолжить работу, даже если записать его не удалось.
     */
    CloudClient(HttpTransport& transport, Credentials credentials, std::function<void(const Credentials&)> on_rotate);

    /// Двухступенчатый вход. Бросает MiFitnessAuthError на любом отклонении
    /// формата: молча продолжать с половиной полей нельзя.
    void login();

    const Credentials& credentials() const { return credentials_; }

    /**
     * @brief Записи одного ключа данных за диапазон дат, все страницы разом.
     *
     * Пагинация идёт по курсору next_key до has_more=false. Повтор курсора и
     * превышение потолка страниц дают MiFitnessProtocolError: бесконечный цикл
     * хуже громкого отказа. Вызов до login() даёт MiFitnessAuthError.
     */
    std::vector<nlohmann::json> fetch_key(std::string_view key,
                                          std::string_view start_date,
                                          std::string_view end_date,
                                          std::optional<std::string_view> region);

    /**
     * @brief Тренировки за диапазон: отдельный эндпоинт, limit 50, поле
     *        sport_records. Правила курсора как у fetch_key.
     */
    std::vector<nlohmann::json> fetch_sport_records(std::string_view start_date, std::string_view end_date);

    /**
     * @brief Суточные отчёты сна своего аккаунта за диапазон дат пробуждения.
     *
     * Отдельный эндпоинт агрегатов с более строгим курсором: нестроковый,
     * пустой или повторный next_key это ошибка протокола, не тихий обрыв.
     * Форма data_list обязана быть массивом.
     */
    std::vector<nlohmann::json> fetch_daily_sleep_reports(std::string_view start_date, std::string_view end_date);

    /**
     * @brief Подписанный POST к облаку. Возвращает поле result конверта.
     *
     * Ненулевой code это отказ: коды авторизации апстрима (401, 403, -6,
     * -10001) дают MiFitnessAuthError, который очередь заданий не ретраит,
     * остальные MiFitnessProtocolError.
     */
    nlohmann::json post_signed(const std::string& base_url, std::string_view api_path, const nlohmann::json& payload);

    /// Потолок страниц пагинации (используется с задачи 5).
    void set_max_pages(int max_pages) { max_pages_ = max_pages; }
    int max_pages() const { return max_pages_; }

    /// База бэкоффа ретраев в миллисекундах. Ноль в тестах: без него каждый
    /// прогон с ретраями спит по секундам.
    void set_retry_backoff_base_ms(int base_ms) { retry_backoff_base_ms_ = base_ms; }

private:
    /// Одна попытка подписанного POST без ретраев.
    nlohmann::json post_signed_once(const std::string& base_url,
                                    std::string_view api_path,
                                    const nlohmann::json& payload);

    HttpTransport& transport_;
    Credentials credentials_;
    std::function<void(const Credentials&)> on_rotate_;
    std::string ssecurity_b64_;
    std::string cookies_;
    int max_pages_ = 200;
    int retry_backoff_base_ms_ = 500;
};

}  // namespace Xiaomi
