/**
 * @file Errors.hpp
 * @brief Два класса отказов протокола Xiaomi, различаемые по поведению.
 *
 * Разделение не косметическое. MiFitnessAuthError означает, что облако не
 * приняло учётные данные: повторять запрос бессмысленно, очередь заданий
 * такую ошибку не ретраит, а отдаёт наружу как требующую вмешательства.
 * MiFitnessProtocolError означает нарушение формата или контракта пагинации:
 * это признак того, что Xiaomi поменял закрытый интерфейс, и его надо видеть
 * отдельной метрикой.
 *
 * Ни в одном из сообщений не должно быть значений токена, ssecurity или
 * полного идентификатора аккаунта: текст ошибки уезжает в логи.
 */

#pragma once

#include <stdexcept>
#include <string>

namespace Xiaomi {

struct MiFitnessAuthError : std::runtime_error {
    explicit MiFitnessAuthError(const std::string& what) : std::runtime_error(what) {}
};

struct MiFitnessProtocolError : std::runtime_error {
    explicit MiFitnessProtocolError(const std::string& what) : std::runtime_error(what) {}
};

}  // namespace Xiaomi
