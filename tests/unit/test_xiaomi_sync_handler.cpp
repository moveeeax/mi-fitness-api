/**
 * @file test_xiaomi_sync_handler.cpp
 * @brief Обработчик xiaomi_sync зарегистрирован среди встроенных.
 *
 * Тот же класс защиты, что у BuiltinHandlersAreRegistered: пропавший или
 * переименованный обработчик молча отправлял бы настоящие задания в DLQ, и
 * ни один другой тест этого не увидел бы.
 */

#include <gtest/gtest.h>

#include "jobs/BuiltinHandlers.hpp"
#include "jobs/Dispatcher.hpp"
#include "jobs/XiaomiSyncHandler.hpp"

TEST(XiaomiSyncHandler, IsRegisteredAmongBuiltins) {
    Jobs::register_builtin_handlers();
    EXPECT_TRUE(Jobs::Dispatcher::get().has_handler(Jobs::XiaomiSync::kJobType));
    EXPECT_STREQ(Jobs::XiaomiSync::kJobType, "xiaomi_sync");
}
