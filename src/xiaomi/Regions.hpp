/**
 * @file Regions.hpp
 * @brief Регионы облака Mi Fitness: хосты и границы суток.
 *
 * Список регионов это кандидаты маршрутизации, а не проверенная поддержка:
 * апстрим прямо оговаривает, что KNOWN_REGIONS ничего не гарантирует.
 */

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "xiaomi/Errors.hpp"

namespace Xiaomi {

inline constexpr std::array<std::string_view, 6> kKnownRegions = {"ru", "cn", "de", "i2", "sg", "us"};

/// Пустая строка это синоним cn. Остальное сверяется со списком кандидатов:
/// значение попадает в имя хоста, и произвольная строка увела бы запрос с
/// куками сессии на чужой домен.
inline bool is_known_region(std::string_view region) {
    if (region.empty()) {
        return true;
    }
    for (const auto candidate : kKnownRegions) {
        if (region == candidate) {
            return true;
        }
    }
    return false;
}

/// Регион cn и пустая строка живут на hlth.io.mi.com, остальные на поддомене.
inline std::string host_for_region(std::string_view region) {
    if (region.empty() || region == "cn") {
        return "https://hlth.io.mi.com";
    }
    return "https://" + std::string(region) + ".hlth.io.mi.com";
}

namespace detail {

/// Строгий разбор YYYY-MM-DD. Всё остальное это ошибка вызывающего, а не
/// повод молча угадать формат.
inline std::chrono::sys_days parse_date(std::string_view text) {
    const auto bad = [&] { return MiFitnessProtocolError("date must be YYYY-MM-DD and a real calendar day"); };
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        throw bad();
    }
    for (const std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u}) {
        if (text[i] < '0' || text[i] > '9') {
            throw bad();
        }
    }
    const auto number = [&](std::size_t from, std::size_t count) {
        int value = 0;
        for (std::size_t i = from; i < from + count; ++i) {
            value = value * 10 + (text[i] - '0');
        }
        return value;
    };
    const std::chrono::year_month_day date{std::chrono::year(number(0, 4)),
                                           std::chrono::month(static_cast<unsigned>(number(5, 2))),
                                           std::chrono::day(static_cast<unsigned>(number(8, 2)))};
    if (!date.ok()) {
        throw bad();
    }
    return std::chrono::sys_days(date);
}

}  // namespace detail

/**
 * @brief Границы диапазона дат в секундах эпохи, включительно с обеих сторон.
 *
 * Регион cn считает сутки в UTC+8 жёстко, остальные в UTC: так делает апстрим,
 * и ошибка здесь сдвигает сутки и портит суточные агрегаты.
 */
inline std::pair<std::int64_t, std::int64_t> range_to_timestamps(std::string_view start_date,
                                                                 std::string_view end_date,
                                                                 std::string_view region) {
    const std::int64_t offset = (region.empty() || region == "cn") ? 8 * 3600 : 0;
    const std::int64_t start_days = detail::parse_date(start_date).time_since_epoch().count();
    const std::int64_t end_days = detail::parse_date(end_date).time_since_epoch().count();
    if (start_days > end_days) {
        throw MiFitnessProtocolError("start date is after end date");
    }
    return {start_days * 86400 - offset, end_days * 86400 + 86399 - offset};
}

}  // namespace Xiaomi
