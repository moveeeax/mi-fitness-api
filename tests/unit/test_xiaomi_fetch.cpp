/**
 * @file test_xiaomi_fetch.cpp
 * @brief Хосты регионов, границы диапазона, подпись запроса, пагинация, конверт.
 */

#include <string>

#include <gtest/gtest.h>

#include "FakeHttpTransport.hpp"
#include "xiaomi/CloudClient.hpp"
#include "xiaomi/Regions.hpp"

namespace {

Xiaomi::Credentials seed() {
    return {"1234567890", std::string(347, 'S'), "cn"};
}

std::string page(const std::string& items, bool has_more, const std::string& next_key) {
    return std::string(R"({"code":0,"result":{"data_list":[)") + items + R"(],"has_more":)" +
           (has_more ? "true" : "false") + R"(,"next_key":)" + next_key + "}}";
}

}  // namespace

TEST(XiaomiRegions, HostMapping) {
    EXPECT_EQ(Xiaomi::host_for_region("cn"), "https://hlth.io.mi.com");
    EXPECT_EQ(Xiaomi::host_for_region(""), "https://hlth.io.mi.com");
    EXPECT_EQ(Xiaomi::host_for_region("de"), "https://de.hlth.io.mi.com");
    EXPECT_EQ(Xiaomi::host_for_region("sg"), "https://sg.hlth.io.mi.com");
    EXPECT_EQ(Xiaomi::kKnownRegions.size(), 6u);
}

// Регион cn считает границы суток в UTC+8, остальные в UTC. Ошибка здесь
// сдвигает сутки и портит суточные агрегаты, поэтому числа выписаны явно.
TEST(XiaomiRegions, RangeBoundsUseRegionOffset) {
    // 2026-09-22 00:00:00 +08:00 == 1790006400, конец суток == 1790092799.
    const auto cn = Xiaomi::range_to_timestamps("2026-09-22", "2026-09-22", "cn");
    EXPECT_EQ(cn.first, 1790006400);
    EXPECT_EQ(cn.second, 1790092799);
    // 2026-09-22 00:00:00 UTC == 1790035200, ровно на восемь часов позже.
    const auto utc = Xiaomi::range_to_timestamps("2026-09-22", "2026-09-22", "de");
    EXPECT_EQ(utc.first, 1790035200);
    EXPECT_EQ(utc.first - cn.first, 8 * 3600);
}

TEST(XiaomiRegions, RangeRejectsMalformedDates) {
    EXPECT_THROW(Xiaomi::range_to_timestamps("22.09.2026", "2026-09-22", "cn"), Xiaomi::MiFitnessProtocolError);
    EXPECT_THROW(Xiaomi::range_to_timestamps("2026-13-01", "2026-13-02", "cn"), Xiaomi::MiFitnessProtocolError);
    EXPECT_THROW(Xiaomi::range_to_timestamps("2026-02-30", "2026-03-01", "cn"), Xiaomi::MiFitnessProtocolError);
}

TEST(XiaomiFetch, PaginatesUntilHasMoreIsFalse) {
    FakeHttpTransport transport;
    transport.reply_login();
    transport.reply_encrypted(page(R"({"a":1})", true, R"("k1")"));
    transport.reply_encrypted(page(R"({"a":2})", false, "null"));
    Xiaomi::CloudClient client(transport, seed(), [](const auto&) {});
    client.login();

    const auto items = client.fetch_key("steps", "2026-09-22", "2026-09-23", std::nullopt);

    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0]["a"], 1);
    EXPECT_EQ(items[1]["a"], 2);
    // Вторая страница несёт курсор первой. Поле data зашифровано, поэтому
    // расшифровываем его так же, как это делает сервер: nonce из тела.
    const auto& second_page = transport.requests().back();
    const std::string nonce = Xiaomi::Crypto::b64_decode(FakeHttpTransport::form_value(second_page.body, "_nonce"));
    const std::string signed_nonce = Xiaomi::Crypto::signed_nonce(FakeHttpTransport::kSsecurityB64, nonce);
    const std::string decrypted = Xiaomi::Crypto::rc4(
        signed_nonce, Xiaomi::Crypto::b64_decode(FakeHttpTransport::form_value(second_page.body, "data")));
    EXPECT_NE(decrypted.find(R"("next_key":"k1")"), std::string::npos) << decrypted;
}

TEST(XiaomiFetch, RepeatedCursorIsTreatedAsALoop) {
    FakeHttpTransport transport;
    transport.reply_login();
    transport.reply_encrypted(page(R"({"a":1})", true, R"("same")"));
    transport.reply_encrypted(page(R"({"a":2})", true, R"("same")"));
    Xiaomi::CloudClient client(transport, seed(), [](const auto&) {});
    client.login();

    EXPECT_THROW(client.fetch_key("steps", "2026-09-22", "2026-09-23", std::nullopt), Xiaomi::MiFitnessProtocolError);
}

TEST(XiaomiFetch, PageCeilingStopsRunawayPagination) {
    FakeHttpTransport transport;
    transport.reply_login();
    for (int i = 0; i < 12; ++i) {
        transport.reply_encrypted(page(R"({"a":1})", true, R"("k)" + std::to_string(i) + R"(")"));
    }
    Xiaomi::CloudClient client(transport, seed(), [](const auto&) {});
    client.login();
    client.set_max_pages(10);

    EXPECT_THROW(client.fetch_key("steps", "2026-01-01", "2026-09-29", std::nullopt), Xiaomi::MiFitnessProtocolError);
    // Логин это два запроса; страниц ушло не больше потолка.
    EXPECT_LE(transport.requests().size(), 12u);
}

TEST(XiaomiFetch, RequestCarriesAllSignatureFields) {
    FakeHttpTransport transport;
    transport.reply_login();
    transport.reply_encrypted(page("", false, "null"));
    Xiaomi::CloudClient client(transport, seed(), [](const auto&) {});
    client.login();
    client.fetch_key("steps", "2026-09-22", "2026-09-22", std::nullopt);

    const auto& request = transport.requests().back();
    EXPECT_EQ(request.method, "POST");
    EXPECT_NE(request.url.find("/app/v1/data/get_fitness_data_by_time"), std::string::npos);
    for (const char* field : {"data", "rc4_hash__", "signature", "_nonce"}) {
        EXPECT_NO_THROW(FakeHttpTransport::form_value(request.body, field)) << field;
    }
    // Полезная нагрузка зашифрована: открытый ключ запроса в теле не виден.
    EXPECT_EQ(request.body.find("start_time"), std::string::npos);
}

// Ненулевой code это отказ сервера, а не пустой результат.
TEST(XiaomiFetch, NonZeroCodeIsAProtocolError) {
    FakeHttpTransport transport;
    transport.reply_login();
    transport.reply_encrypted(R"({"code":1234,"message":"server broke"})");
    Xiaomi::CloudClient client(transport, seed(), [](const auto&) {});
    client.login();

    EXPECT_THROW(client.fetch_key("steps", "2026-09-22", "2026-09-22", std::nullopt), Xiaomi::MiFitnessProtocolError);
}

// Коды авторизации апстрима: 401, 403, -6, -10001. Такой отказ очередь заданий
// не ретраит, поэтому класс ошибки обязан отличаться.
TEST(XiaomiFetch, AuthCodeIsAnAuthError) {
    FakeHttpTransport transport;
    transport.reply_login();
    transport.reply_encrypted(R"({"code":-10001,"message":"auth expired"})");
    Xiaomi::CloudClient client(transport, seed(), [](const auto&) {});
    client.login();

    EXPECT_THROW(client.fetch_key("steps", "2026-09-22", "2026-09-22", std::nullopt), Xiaomi::MiFitnessAuthError);
}

TEST(XiaomiFetch, FetchBeforeLoginIsAnAuthError) {
    FakeHttpTransport transport;
    Xiaomi::CloudClient client(transport, seed(), [](const auto&) {});
    EXPECT_THROW(client.fetch_key("steps", "2026-09-22", "2026-09-22", std::nullopt), Xiaomi::MiFitnessAuthError);
    EXPECT_TRUE(transport.requests().empty());
}
