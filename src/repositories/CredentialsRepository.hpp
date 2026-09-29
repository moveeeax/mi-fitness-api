/**
 * @file CredentialsRepository.hpp
 * @brief Единственная строка учётных данных Xiaomi (миграция 011).
 *
 * Сервис обслуживает один аккаунт, поэтому таблица хранит ровно одну строку, а
 * запись идёт через ON CONFLICT: ротация обновляет её, а не копит историю
 * мёртвых токенов.
 *
 * Ключ шифрования приходит конструктором из окружения и в базу не попадает.
 */

#pragma once

#include <optional>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include "database/Database.hpp"
#include "utils/Config.hpp"
#include "xiaomi/Credentials.hpp"
#include "xiaomi/Crypto.hpp"
#include "xiaomi/Regions.hpp"

namespace Repositories {

class CredentialsRepository {
public:
    explicit CredentialsRepository(std::string key_b64) : key_b64_(std::move(key_b64)) {}

    /// Возвращает пустое значение, когда строки ещё нет: это не ошибка, а
    /// признак того, что учётные данные надо посеять из Secret.
    std::optional<Xiaomi::Credentials> load() {
        return Database::get().execute_read([&](auto& txn) -> std::optional<Xiaomi::Credentials> {
            auto r = txn.exec_params(
                "SELECT user_id, pass_token_sealed, nonce, region FROM xiaomi_credentials "
                "ORDER BY rotated_at DESC LIMIT 1");
            if (r.empty()) {
                return std::nullopt;
            }
            const auto& row = r[0];

            Xiaomi::Sealed sealed;
            sealed.ciphertext = Xiaomi::Crypto::b64_decode(row["pass_token_sealed"].template as<std::string>());
            sealed.nonce = Xiaomi::Crypto::b64_decode(row["nonce"].template as<std::string>());

            Xiaomi::Credentials out;
            out.user_id = row["user_id"].template as<std::string>();
            out.region = row["region"].template as<std::string>();
            out.pass_token = Xiaomi::unseal(sealed, key_b64_);
            validate_identity(out);
            // Проверка и на чтении тоже: значение могло попасть в базу мимо
            // store, и тогда отказ должен случиться здесь, а не на запросе к
            // Xiaomi с невнятной ошибкой кодирования заголовка.
            Xiaomi::validate_pass_token(out.pass_token);
            return out;
        });
    }

    /// Пишет или обновляет единственную строку. Токен проверяется до записи:
    /// мусор в базе стоит дороже, чем отказ на входе.
    void store(const Xiaomi::Credentials& credentials) {
        validate_identity(credentials);
        Xiaomi::validate_pass_token(credentials.pass_token);
        const Xiaomi::Sealed sealed = Xiaomi::seal(credentials.pass_token, key_b64_);
        const std::string ciphertext_b64 = Xiaomi::Crypto::b64_encode(sealed.ciphertext);
        const std::string nonce_b64 = Xiaomi::Crypto::b64_encode(sealed.nonce);

        Database::get().execute_write([&](auto& txn) {
            txn.exec_params(
                "INSERT INTO xiaomi_credentials (user_id, pass_token_sealed, nonce, region) "
                "VALUES ($1, $2, $3, $4) "
                "ON CONFLICT (user_id) DO UPDATE SET "
                "pass_token_sealed = EXCLUDED.pass_token_sealed, "
                "nonce = EXCLUDED.nonce, "
                "region = EXCLUDED.region, "
                "rotated_at = now()",
                credentials.user_id,
                ciphertext_b64,
                nonce_b64,
                credentials.region);
            return true;
        });
    }

private:
    /// user_id уходит в заголовок Cookie рядом с токеном, region в имя хоста
    /// облака. CRLF в первом это инъекция заголовка, произвольная строка во
    /// втором увела бы куки запроса на чужой домен (находка обзора 2).
    static void validate_identity(const Xiaomi::Credentials& credentials) {
        Xiaomi::validate_pass_token(credentials.user_id);
        if (!Xiaomi::is_known_region(credentials.region)) {
            throw Xiaomi::MiFitnessAuthError("region is not one of the known candidates");
        }
    }

    std::string key_b64_;
};

/**
 * @brief Посев учётных данных Xiaomi из конфигурации при старте сервиса.
 *
 * Secret кластера это только сид. Ротированный токен в базе главнее: Xiaomi
 * выдал его позже, чем Secret был создан, и перетирание откатило бы сессию к
 * мёртвому значению. Аварийный рычаг xiaomi.reseed / MI_FITNESS_RESEED
 * перетирает строку сознательно: токен в базе умер, владелец положил свежий.
 *
 * Возвращает true, когда строка записана. Неполный сид это не ошибка, а
 * "не настроено": сервис поднимется и будет отвечать not_configured.
 * Нечитаемая строка при выключенном reseed роняет старт нарочно: это значит,
 * что MI_FITNESS_TOKEN_KEY не тот, и молча перетирать ротированный токен
 * хуже, чем не подняться.
 */
inline bool seed_xiaomi_credentials_if_missing() {
    if (!Config::is_initialized()) {
        return false;
    }
    auto& cfg = Config::get();
    const std::string token_key = cfg.get<std::string>("xiaomi.token_key", "MI_FITNESS_TOKEN_KEY", "");
    const std::string user_id = cfg.get<std::string>("xiaomi.user_id", "MI_FITNESS_USER_ID", "");
    const std::string pass_token = cfg.get<std::string>("xiaomi.pass_token", "MI_FITNESS_PASS_TOKEN", "");
    const std::string region = cfg.get<std::string>("xiaomi.region", "MI_FITNESS_REGION", "cn");
    const bool reseed = cfg.get<bool>("xiaomi.reseed", "MI_FITNESS_RESEED", false);

    if (token_key.empty() || user_id.empty() || pass_token.empty()) {
        spdlog::info("xiaomi: credential seeding skipped, seed values are not configured");
        return false;
    }

    CredentialsRepository repository(token_key);
    if (!reseed && repository.load().has_value()) {
        return false;
    }
    repository.store({user_id, pass_token, region});
    spdlog::info("xiaomi: credentials seeded for account {} (reseed={})", Xiaomi::mask_account_id(user_id), reseed);
    return true;
}

}  // namespace Repositories
