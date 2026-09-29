/**
 * @file DataKeys.hpp
 * @brief Ключи типов данных, которые понимает облако Mi Fitness.
 *
 * Список повторяет апстрим. Неизвестный ключ отсекается до сети: облако на
 * него ответит пустотой или ошибкой, и не отличить опечатку от отсутствия
 * данных.
 */

#pragma once

#include <algorithm>
#include <array>
#include <string_view>

namespace Xiaomi {

inline constexpr std::array<std::string_view, 9> kDataKeys = {"steps",
                                                              "calories",
                                                              "sleep",
                                                              "weight",
                                                              "heart_rate",
                                                              "spo2",
                                                              "stress",
                                                              "resting_heart_rate",
                                                              "abnormal_heart_beat"};

inline bool is_known_data_key(std::string_view key) {
    return std::find(kDataKeys.begin(), kDataKeys.end(), key) != kDataKeys.end();
}

}  // namespace Xiaomi
