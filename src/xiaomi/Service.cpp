/**
 * @file Service.cpp
 * @brief Тела шва: боевой транспорт и чтение настроек.
 */

#include "xiaomi/Service.hpp"

#include "utils/Config.hpp"
#include "xiaomi/CurlTransport.hpp"

namespace Xiaomi::Service {

namespace {

HttpTransport*& override_slot() {
    static HttpTransport* slot = nullptr;
    return slot;
}

}  // namespace

HttpTransport& transport() {
    if (override_slot() != nullptr) {
        return *override_slot();
    }
    static CurlTransport production;
    return production;
}

void install_for_testing(HttpTransport* transport) {
    override_slot() = transport;
}

std::string token_key_b64() {
    if (!Config::is_initialized()) {
        return {};
    }
    return Config::get().get<std::string>("xiaomi.token_key", "MI_FITNESS_TOKEN_KEY", "");
}

}  // namespace Xiaomi::Service
