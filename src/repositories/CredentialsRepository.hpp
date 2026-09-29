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

#include "database/Database.hpp"
#include "xiaomi/Credentials.hpp"
#include "xiaomi/Crypto.hpp"

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
    std::string key_b64_;
};

}  // namespace Repositories
