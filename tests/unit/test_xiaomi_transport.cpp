/**
 * @file test_xiaomi_transport.cpp
 * @brief Таймаут HTTP-запроса к облаку читается из конфига.
 *
 * Живой бэкфил июля-сентября упал на «Timeout was reached»: жёсткие 20 секунд
 * CurlTransport меньше времени ответа облака на глубокие диапазоны. Ручка
 * xiaomi.http_timeout_seconds (MI_FITNESS_HTTP_TIMEOUT) поднимает предел без
 * пересборки; дефолт остаётся прежним.
 */

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "test_helpers.hpp"
#include "utils/Config.hpp"
#include "xiaomi/CurlTransport.hpp"
#include "xiaomi/Service.hpp"

namespace {

constexpr const char* kConfigFile = "xiaomi_timeout_config.json";

}  // namespace

class XiaomiTransportTimeoutTest : public ::testing::Test {
protected:
    void SetUp() override {
        unsetenv("MI_FITNESS_HTTP_TIMEOUT");
        if (Config::is_initialized()) {
            Config::shutdown();
        }
    }

    void TearDown() override {
        if (Config::is_initialized()) {
            Config::shutdown();
        }
        std::filesystem::remove(kConfigFile);
        TestHelpers::reset_all_globals();
    }
};

TEST_F(XiaomiTransportTimeoutTest, CurlTransportCarriesExplicitTimeout) {
    Xiaomi::CurlTransport transport(120);
    EXPECT_EQ(transport.timeout_seconds(), 120);
}

TEST_F(XiaomiTransportTimeoutTest, DefaultsToTwentySecondsWithoutConfig) {
    ASSERT_FALSE(Config::is_initialized());
    EXPECT_EQ(Xiaomi::Service::http_timeout_seconds(), 20);
}

TEST_F(XiaomiTransportTimeoutTest, ReadsTimeoutFromConfig) {
    std::ofstream(kConfigFile) << R"({"xiaomi": {"http_timeout_seconds": 120}})";
    Config::initialize(kConfigFile);
    EXPECT_EQ(Xiaomi::Service::http_timeout_seconds(), 120);
}
