/**
 * @file test_csv_escape.cpp
 * @brief Экранирование CSV: формулы режутся как у моста, числа не портятся.
 *
 * Обзор фазы 3, Important 1-2: ведущий пробел/таб/CR обходили защиту от
 * формул, а апостроф прилипал к любому значению, включая отрицательные
 * числа. Правила моста (_escape_csv_value): lstrip перед проверкой,
 * префиксы = + - @ TAB CR, экранируются только строковые значения.
 */

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/DataController.hpp"

using nlohmann::json;

TEST(CsvEscape, LeadingWhitespaceDoesNotBypassTheFormulaGuard) {
    const json rows = json::array({{{"a", " =HYPERLINK(\"x\")"}, {"b", "\t=1+1"}, {"c", "\r@cmd"}}});
    const std::string csv = Api::DataController::to_csv(rows);
    EXPECT_EQ(csv.find(", =HYPERLINK"), std::string::npos) << csv;
    EXPECT_NE(csv.find("' =HYPERLINK"), std::string::npos) << csv;
    EXPECT_NE(csv.find("'\t=1+1"), std::string::npos) << csv;
    EXPECT_NE(csv.find("'\r@cmd"), std::string::npos) << csv;
}

TEST(CsvEscape, NumbersAreNotMangled) {
    const json rows = json::array({{{"delta", -3.5}, {"n", -7}}});
    const std::string csv = Api::DataController::to_csv(rows);
    EXPECT_EQ(csv.find("'-3.5"), std::string::npos) << csv;
    EXPECT_NE(csv.find("-3.5"), std::string::npos) << csv;
    EXPECT_NE(csv.find("-7"), std::string::npos) << csv;
}

TEST(CsvEscape, EmptyRowsStillCarryNothingButNoCrash) {
    EXPECT_EQ(Api::DataController::to_csv(json::array()), "");
}
