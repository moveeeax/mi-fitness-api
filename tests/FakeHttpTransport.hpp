/**
 * @file FakeHttpTransport.hpp
 * @brief Подделка HTTP-транспорта: весь клиент облака проверяется без сети.
 *
 * Ответы выдаются в том порядке, в котором их положили. Лишний запрос, на
 * который ответа не заготовлено, это громкий отказ, а не пустой ответ: молчащая
 * подделка прячет как раз те ошибки, ради которых тест писался.
 */

#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include "xiaomi/Crypto.hpp"
#include "xiaomi/HttpTransport.hpp"

class FakeHttpTransport : public Xiaomi::HttpTransport {
public:
    /// ssecurity, который отдаёт reply_login: "secret-material!" в base64.
    static constexpr const char* kSsecurityB64 = "c2VjcmV0LW1hdGVyaWFsIQ==";

    void reply(Xiaomi::HttpResponse response) { queued_.push_back({std::move(response), false, {}}); }

    /// Успешный двухступенчатый логин: ответ с полями и редирект с кукой.
    void reply_login(const std::string& rotated_token = "NEWTOKEN") {
        reply({200,
               std::string("&&&START&&&") + R"({"passToken":")" + rotated_token +
                   R"(","userId":1234567890,"ssecurity":")" + kSsecurityB64 +
                   R"(","location":"https://account.xiaomi.com/pass/end"})",
               {}});
        reply({200, "", {{"set-cookie", "serviceToken=abc; Path=/"}}});
    }

    /// Падение транспорта: send бросает MiFitnessProtocolError с этим текстом,
    /// как CurlTransport на таймауте или сетевой ошибке curl.
    void reply_transport_error(std::string message) {
        Queued item;
        item.throw_message = std::move(message);
        queued_.push_back(std::move(item));
    }

    /// Зашифрованный ответ данных. Шифруется в момент запроса: signed_nonce
    /// зависит от _nonce, который клиент кладёт в тело, и до прихода запроса
    /// подделке неизвестен. Используются те же функции крипты, что и в клиенте,
    /// поэтому удачная расшифровка заодно перекрёстно их проверяет.
    void reply_encrypted(std::string envelope_json) {
        queued_.push_back({{200, "", {}}, true, std::move(envelope_json)});
    }

    Xiaomi::HttpResponse send(const Xiaomi::HttpRequest& request) override {
        requests_.push_back(request);
        if (queued_.empty()) {
            throw std::runtime_error("FakeHttpTransport: unexpected request #" + std::to_string(requests_.size()) +
                                     " to " + request.url);
        }
        Queued item = queued_.front();
        queued_.erase(queued_.begin());
        if (!item.throw_message.empty()) {
            throw Xiaomi::MiFitnessProtocolError("Xiaomi request failed: " + item.throw_message);
        }
        if (item.encrypt) {
            const std::string nonce = Xiaomi::Crypto::b64_decode(form_value(request.body, "_nonce"));
            const std::string signed_nonce = Xiaomi::Crypto::signed_nonce(kSsecurityB64, nonce);
            item.response.body = Xiaomi::Crypto::b64_encode(Xiaomi::Crypto::rc4(signed_nonce, item.plaintext));
        }
        return item.response;
    }

    const std::vector<Xiaomi::HttpRequest>& requests() const { return requests_; }

    /// Значение поля form-urlencoded тела с процентным декодированием.
    static std::string form_value(const std::string& body, const std::string& name) {
        const std::string needle = name + "=";
        std::size_t at = 0;
        while (at < body.size()) {
            const std::size_t end = body.find('&', at);
            const std::string pair = body.substr(at, end == std::string::npos ? std::string::npos : end - at);
            if (pair.rfind(needle, 0) == 0) {
                return percent_decode(pair.substr(needle.size()));
            }
            if (end == std::string::npos) {
                break;
            }
            at = end + 1;
        }
        throw std::runtime_error("FakeHttpTransport: form field not found: " + name);
    }

private:
    static std::string percent_decode(const std::string& value) {
        std::string out;
        out.reserve(value.size());
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '%' && i + 2 < value.size()) {
                out.push_back(static_cast<char>(std::stoi(value.substr(i + 1, 2), nullptr, 16)));
                i += 2;
            } else if (value[i] == '+') {
                out.push_back(' ');
            } else {
                out.push_back(value[i]);
            }
        }
        return out;
    }

    struct Queued {
        Xiaomi::HttpResponse response;
        bool encrypt = false;
        std::string plaintext;
        std::string throw_message;
    };

    std::vector<Xiaomi::HttpRequest> requests_;
    std::vector<Queued> queued_;
};
