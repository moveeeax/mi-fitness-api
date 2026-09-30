/**
 * @file test_sync_service.cpp
 * @brief Синк целиком: облако это подделка, база настоящая.
 *
 * Review Focus плана: идемпотентность повторного прохода, изоляция провала
 * типа, класс auth-ошибки, взаимное исключение запусков, обновление sync_state.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "FakeHttpTransport.hpp"
#include "database/Database.hpp"
#include "repositories/SyncRunRepository.hpp"
#include "sync/SyncService.hpp"
#include "test_helpers.hpp"
#include "xiaomi/Regions.hpp"

namespace {

using nlohmann::json;

constexpr long long kNoon = 1790049600;  // 2026-09-22T12:00:00+08:00

class SyncServiceTest : public TestHelpers::CoreBackedTest {
protected:
    FakeHttpTransport transport;

    std::string config_file_name() const override { return "sync_service_test_config.json"; }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Database::get().execute_write([](auto& txn) {
            txn.exec(
                "TRUNCATE TABLE daily_activity, sleep_sessions, workouts, body_measurements, "
                "heart_rate_samples, spo2_samples, stress_samples, abnormal_heart_beat_events, "
                "sync_state, sync_runs");
            return true;
        });
    }

    static std::string steps_page(int steps) {
        const json record{{"time", kNoon},
                          {"zone_offset", 28800},
                          {"zone_name", "Asia/Shanghai"},
                          {"value", json{{"steps", steps}, {"distance", 10.0}, {"calories", 1.0}}.dump()}};
        return json{{"code", 0}, {"result", {{"data_list", json::array({record})}, {"has_more", false}}}}.dump();
    }

    static std::string steps_page_at(long long time, int zone_offset, int steps) {
        const json record{{"time", time},
                          {"zone_offset", zone_offset},
                          {"value", json{{"steps", steps}, {"distance", 10.0}, {"calories", 1.0}}.dump()}};
        return json{{"code", 0}, {"result", {{"data_list", json::array({record})}, {"has_more", false}}}}.dump();
    }

    static std::string weight_page() {
        const json record{{"time", kNoon}, {"zone_offset", 28800}, {"value", json{{"weight", 91.9}}.dump()}};
        return json{{"code", 0}, {"result", {{"data_list", json::array({record})}, {"has_more", false}}}}.dump();
    }

    static std::string sleep_page(long long bed, long long wake) {
        const json value{{"bedtime", bed}, {"wake_up_time", wake}};
        const json record{{"time", wake},
                          {"zone_offset", 28800},
                          {"zone_name", "Asia/Shanghai"},
                          {"sid", "band-1"},
                          {"value", value.dump()}};
        return json{{"code", 0}, {"result", {{"data_list", json::array({record})}, {"has_more", false}}}}.dump();
    }

    static std::string empty_page() {
        return json{{"code", 0}, {"result", {{"data_list", json::array()}, {"has_more", false}}}}.dump();
    }

    Sync::SyncService service() { return Sync::SyncService(transport, {"1234567890", std::string(347, 'S'), "cn"}); }
};

}  // namespace

// Сутки, разрезанные границей куска, собираются целиком. Границы кусков идут
// в поясе региона (+08 для cn), устройство живёт в +07: вечерние минуты дня X
// попадают в следующий кусок, и построчный upsert по кускам перезатирал
// полный агрегат дня часовым огрызком. Живой бэкфил терял так каждый
// седьмой день (сверка с Python-мостом, отчёт 2026-09).
TEST_F(SyncServiceTest, ChunkBoundaryDayIsAggregatedAcrossChunks) {
    Repositories::SyncRunRepository runs;
    transport.reply_login();
    // Кусок 1 (2026-09-22..28): полный день 28-го, полдень +08.
    transport.reply_encrypted(steps_page_at(1790568000, 28800, 7000));  // steps
    transport.reply_encrypted(empty_page());                            // calories
    // Кусок 2 (2026-09-29..30): вечерняя минута того же 28-го в +07.
    transport.reply_encrypted(steps_page_at(1790613000, 25200, 226));  // steps
    transport.reply_encrypted(empty_page());                           // calories

    const long id = runs.create("2026-09-22", "2026-09-30", {"daily_activity"});
    service().run(id, "2026-09-22", "2026-09-30", {"daily_activity"});

    const auto row = runs.get(id);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ((*row)["status"], "succeeded");
    const long steps = Database::get().execute_read([](auto& txn) {
        auto r = txn.exec("SELECT steps FROM daily_activity WHERE date = '2026-09-28'");
        return r.empty() ? -1L : r[0][0].template as<long>();
    });
    EXPECT_EQ(steps, 7226);
}

// Находки финального обзора плана 2. Пять поведений ниже закрывают
// Critical 1-2 и Important 3-5 отчёта ревьюера.

// Critical 1: сутки на краю запрошенного диапазона заведомо частичные, их
// нельзя upsert-ить: окно режется в поясе региона, и вечерняя минута
// предыдущего дня попадает внутрь. Полный день в базе не должен затираться.
TEST_F(SyncServiceTest, RangeEdgeDayOutsideRequestIsNotUpserted) {
    Repositories::SyncRunRepository runs;
    transport.reply_login();
    transport.reply_encrypted(steps_page_at(1790568000, 28800, 7000));  // кусок 22..28
    transport.reply_encrypted(empty_page());
    transport.reply_encrypted(steps_page_at(1790613000, 25200, 226));  // кусок 29..30
    transport.reply_encrypted(empty_page());
    const long full = runs.create("2026-09-22", "2026-09-30", {"daily_activity"});
    service().run(full, "2026-09-22", "2026-09-30", {"daily_activity"});

    // Инкрементальный запуск со следующего дня: та же вечерняя минута 28-го
    // входит в окно 29..30, но день 28-й вне запроса.
    transport.reply_login();
    transport.reply_encrypted(steps_page_at(1790613000, 25200, 226));
    transport.reply_encrypted(empty_page());
    const long tail = runs.create("2026-09-29", "2026-09-30", {"daily_activity"});
    service().run(tail, "2026-09-29", "2026-09-30", {"daily_activity"});

    const long steps = Database::get().execute_read([](auto& txn) {
        auto r = txn.exec("SELECT steps FROM daily_activity WHERE date = '2026-09-28'");
        return r.empty() ? -1L : r[0][0].template as<long>();
    });
    EXPECT_EQ(steps, 7226);
}

// Important 4: закрытый запуск не оживает от повторной доставки задания.
TEST_F(SyncServiceTest, FinishedRunCannotBeRestarted) {
    Repositories::SyncRunRepository runs;
    transport.reply_login();
    transport.reply_encrypted(steps_page(100));
    transport.reply_encrypted(empty_page());
    const long id = runs.create("2026-09-22", "2026-09-22", {"daily_activity"});
    service().run(id, "2026-09-22", "2026-09-22", {"daily_activity"});

    // Очередь подделки пуста: повторный заход обязан закончиться до сети.
    const auto second = service().run(id, "2026-09-22", "2026-09-22", {"daily_activity"});

    EXPECT_TRUE(second.contains("skipped_reason"));
    const auto row = runs.get(id);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ((*row)["status"], "succeeded");
}

// Critical 2: зависшая строка running (воркер убит посреди синка) не должна
// глушить все будущие запуски. Протухшая строка помечается interrupted.
TEST_F(SyncServiceTest, StaleRunningRunIsInterruptedAndReleasesTheMutex) {
    Repositories::SyncRunRepository runs;
    Database::get().execute_write([](auto& txn) {
        txn.exec(
            "INSERT INTO sync_runs (status, started_at, requested_start, requested_end, data_types) "
            "VALUES ('running', now() - interval '6 hours', '2026-09-01', '2026-09-02', '{daily_activity}')");
        return true;
    });

    transport.reply_login();
    transport.reply_encrypted(steps_page(100));
    transport.reply_encrypted(empty_page());
    const long id = runs.create("2026-09-22", "2026-09-22", {"daily_activity"});
    const auto result = service().run(id, "2026-09-22", "2026-09-22", {"daily_activity"});

    EXPECT_FALSE(result.contains("skipped_reason"));
    const auto row = runs.get(id);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ((*row)["status"], "succeeded");
    const std::string stale = Database::get().execute_read([](auto& txn) {
        auto r = txn.exec("SELECT status FROM sync_runs WHERE requested_start = '2026-09-01'");
        return r[0][0].template as<std::string>();
    });
    EXPECT_EQ(stale, "interrupted");
}

// Important 3: окно суточных отчётов сна берёт день запаса с обеих сторон,
// как эталон: пояс отчёта может не совпадать с поясом региона.
TEST_F(SyncServiceTest, SleepReportWindowHasOneDayMargin) {
    Repositories::SyncRunRepository runs;
    transport.reply_login();
    // Сессия без оценки, пробуждение 2026-09-24 07:00 +08.
    transport.reply_encrypted(sleep_page(1790172000, 1790204400));
    transport.reply_encrypted(empty_page());  // отчёты: пусто

    const long id = runs.create("2026-09-22", "2026-09-28", {"sleep"});
    service().run(id, "2026-09-22", "2026-09-28", {"sleep"});

    // Запрос отчётов: четвёртый запрос (два логина, выборка сна, отчёты).
    ASSERT_EQ(transport.requests().size(), 4u);
    const auto& reports_req = transport.requests().back();
    const std::string nonce = Xiaomi::Crypto::b64_decode(FakeHttpTransport::form_value(reports_req.body, "_nonce"));
    const std::string signed_nonce = Xiaomi::Crypto::signed_nonce(FakeHttpTransport::kSsecurityB64, nonce);
    const std::string decrypted = Xiaomi::Crypto::rc4(
        signed_nonce, Xiaomi::Crypto::b64_decode(FakeHttpTransport::form_value(reports_req.body, "data")));
    const auto payload = json::parse(decrypted);
    const auto expected = Xiaomi::range_to_timestamps("2026-09-23", "2026-09-25", "cn");
    EXPECT_EQ(payload["start_time"].get<long long>(), expected.first);
    EXPECT_EQ(payload["end_time"].get<long long>(), expected.second);
}

// Important 5: отказ авторизации на логине закрывает остальные типы без
// обращения к облаку. Повторные входы с мёртвым токеном сжигают попытки.
TEST_F(SyncServiceTest, AuthFailureOnLoginStopsFurtherLoginAttempts) {
    Repositories::SyncRunRepository runs;
    transport.reply({401, "", {}});

    const long id = runs.create("2026-09-22", "2026-09-22", {"daily_activity", "sleep"});
    service().run(id, "2026-09-22", "2026-09-22", {"daily_activity", "sleep"});

    const auto row = runs.get(id);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ((*row)["status"], "failed");
    EXPECT_EQ((*row)["result"]["daily_activity"]["error"], "auth");
    EXPECT_EQ((*row)["result"]["sleep"]["error"], "auth");
    // Один запрос на весь запуск: второй тип к сети не ходил.
    EXPECT_EQ(transport.requests().size(), 1u);
}

TEST_F(SyncServiceTest, RepeatRunGivesAddedThenUpdated) {
    Repositories::SyncRunRepository runs;

    transport.reply_login();
    transport.reply_encrypted(steps_page(100));  // steps
    transport.reply_encrypted(empty_page());     // calories
    const long first = runs.create("2026-09-22", "2026-09-22", {"daily_activity"});
    service().run(first, "2026-09-22", "2026-09-22", {"daily_activity"});
    auto row = runs.get(first);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ((*row)["status"], "succeeded");
    EXPECT_EQ((*row)["result"]["daily_activity"]["added"], 1);

    transport.reply_login();
    transport.reply_encrypted(steps_page(100));
    transport.reply_encrypted(empty_page());
    const long second = runs.create("2026-09-22", "2026-09-22", {"daily_activity"});
    service().run(second, "2026-09-22", "2026-09-22", {"daily_activity"});
    row = runs.get(second);
    EXPECT_EQ((*row)["result"]["daily_activity"]["added"], 0);
    EXPECT_EQ((*row)["result"]["daily_activity"]["updated"], 1);
}

// Провал одного типа не отменяет остальные, а его класс ошибки виден в журнале.
TEST_F(SyncServiceTest, FailedTypeDoesNotStopTheRest) {
    Repositories::SyncRunRepository runs;
    transport.reply_login();
    // 400 не ретраится: 5xx клиент теперь повторяет и съел бы ответы соседа.
    transport.reply({400, "boom", {}});        // sleep: ошибка протокола
    transport.reply_encrypted(weight_page());  // body_measurements работает

    const long id = runs.create("2026-09-22", "2026-09-22", {"sleep", "body_measurements"});
    service().run(id, "2026-09-22", "2026-09-22", {"sleep", "body_measurements"});

    const auto row = runs.get(id);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ((*row)["status"], "failed");
    EXPECT_EQ((*row)["result"]["sleep"]["error"], "protocol");
    EXPECT_EQ((*row)["result"]["body_measurements"]["added"], 1);
}

// Отказ авторизации помечается отдельно: он лечится свежим токеном, и очередь
// не должна его ретраить.
TEST_F(SyncServiceTest, AuthCodeIsClassifiedAsAuth) {
    Repositories::SyncRunRepository runs;
    transport.reply_login();
    transport.reply_encrypted(R"({"code":-10001,"message":"expired"})");
    transport.reply_encrypted(weight_page());

    const long id = runs.create("2026-09-22", "2026-09-22", {"daily_activity", "body_measurements"});
    service().run(id, "2026-09-22", "2026-09-22", {"daily_activity", "body_measurements"});

    const auto row = runs.get(id);
    EXPECT_EQ((*row)["result"]["daily_activity"]["error"], "auth");
    EXPECT_EQ((*row)["result"]["body_measurements"]["added"], 1);
}

// Вторая строка running невозможна: переход queued -> running атомарен через
// частичный уникальный индекс, конкурент честно завершается skipped.
TEST_F(SyncServiceTest, ConcurrentRunIsSkipped) {
    Repositories::SyncRunRepository runs;
    Database::get().execute_write([](auto& txn) {
        txn.exec(
            "INSERT INTO sync_runs (status, requested_start, requested_end) "
            "VALUES ('running', '2026-09-01', '2026-09-07')");
        return true;
    });

    const long id = runs.create("2026-09-22", "2026-09-22", {"daily_activity"});
    service().run(id, "2026-09-22", "2026-09-22", {"daily_activity"});

    const auto row = runs.get(id);
    EXPECT_EQ((*row)["status"], "skipped");
    EXPECT_TRUE(transport.requests().empty()) << "до облака дойти не должно";
}

TEST_F(SyncServiceTest, SyncStateIsUpdatedPerType) {
    Repositories::SyncRunRepository runs;
    transport.reply_login();
    transport.reply_encrypted(steps_page(100));
    transport.reply_encrypted(empty_page());

    const long id = runs.create("2026-09-22", "2026-09-22", {"daily_activity"});
    service().run(id, "2026-09-22", "2026-09-22", {"daily_activity"});

    const bool has_state = Database::get().execute_read([](auto& txn) {
        auto r = txn.exec(
            "SELECT last_sync_at IS NOT NULL AND records_count = 1 FROM sync_state "
            "WHERE data_type = 'daily_activity'");
        return !r.empty() && r[0][0].template as<bool>();
    });
    EXPECT_TRUE(has_state);
}
