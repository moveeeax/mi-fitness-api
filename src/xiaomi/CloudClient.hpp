/**
 * @file CloudClient.hpp
 * @brief Клиент закрытого облака Mi Fitness: логин и подписанные запросы.
 *
 * Знает HTTP и подпись, не знает ни домена, ни базы. Нормализация записей и
 * хранение живут выше по стеку.
 */

#pragma once

#include <functional>
#include <string>
#include <string_view>

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

    /// Потолок страниц пагинации (используется с задачи 5).
    void set_max_pages(int max_pages) { max_pages_ = max_pages; }
    int max_pages() const { return max_pages_; }

private:
    HttpTransport& transport_;
    Credentials credentials_;
    std::function<void(const Credentials&)> on_rotate_;
    std::string ssecurity_b64_;
    std::string cookies_;
    int max_pages_ = 200;
};

}  // namespace Xiaomi
