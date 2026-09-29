/**
 * @file FakeHttpTransport.hpp
 * @brief Подделка HTTP-транспорта: весь клиент облака проверяется без сети.
 *
 * Ответы выдаются в том порядке, в котором их положили. Лишний запрос, на
 * который ответа не заготовлено, это громкий отказ, а не пустой ответ: молчащая
 * подделка прячет как раз те ошибки, ради которых тест писался.
 */

#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "xiaomi/HttpTransport.hpp"

class FakeHttpTransport : public Xiaomi::HttpTransport {
public:
    void reply(Xiaomi::HttpResponse response) { queued_.push_back(std::move(response)); }

    /// Успешный двухступенчатый логин: ответ с полями и редирект с кукой.
    void reply_login(const std::string& rotated_token = "NEWTOKEN") {
        reply({200,
               std::string("&&&START&&&") + R"({"passToken":")" + rotated_token +
                   R"(","userId":1234567890,"ssecurity":"c2VjcmV0LW1hdGVyaWFsIQ==",)" +
                   R"("location":"https://account.xiaomi.com/pass/end"})",
               {}});
        reply({200, "", {{"set-cookie", "serviceToken=abc; Path=/"}}});
    }

    Xiaomi::HttpResponse send(const Xiaomi::HttpRequest& request) override {
        requests_.push_back(request);
        if (queued_.empty()) {
            throw std::runtime_error("FakeHttpTransport: unexpected request #" + std::to_string(requests_.size()) +
                                     " to " + request.url);
        }
        Xiaomi::HttpResponse response = queued_.front();
        queued_.erase(queued_.begin());
        return response;
    }

    const std::vector<Xiaomi::HttpRequest>& requests() const { return requests_; }

private:
    std::vector<Xiaomi::HttpRequest> requests_;
    std::vector<Xiaomi::HttpResponse> queued_;
};
