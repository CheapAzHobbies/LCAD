// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/core/Expression.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <map>
#include <string>

using namespace lcad::core;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinRel;

namespace {

const double pi = 3.14159265358979323846;

Lookup table(const std::map<std::string, double>& values) {
    return [values](const std::string& name) -> std::optional<double> {
        const auto found = values.find(name);
        if (found == values.end()) {
            return std::nullopt;
        }
        return found->second;
    };
}

} // namespace

TEST_CASE("Expressions are worked out with the usual precedence", "[core][expression]") {
    CHECK(evaluate("40") == 40.0);
    CHECK(evaluate("-2.5") == -2.5);
    CHECK(evaluate(" 1 + 2 * 3 ") == 7.0);
    CHECK(evaluate("(1 + 2) * 3") == 9.0);
    CHECK(evaluate("10 - 4 - 3") == 3.0);
    CHECK(evaluate("24 / 4 / 2") == 3.0);
    CHECK(evaluate("2 ^ 3 ^ 2") == 512.0);
    CHECK(evaluate("-2 ^ 2") == 4.0);
    CHECK(evaluate("1e-3 * 1000") == 1.0);
    CHECK(evaluate(".5 + .25") == 0.75);
    CHECK_THAT(evaluate("pi"), WithinRel(pi, 1e-15));
}

TEST_CASE("Units are converted to millimeters and degrees", "[core][expression]") {
    CHECK(evaluate("40 mm") == 40.0);
    CHECK(evaluate("4cm") == 40.0);
    CHECK(evaluate("0.04 m") == 40.0);
    CHECK_THAT(evaluate("1 in"), WithinRel(25.4, 1e-15));
    CHECK_THAT(evaluate("1 ft + 1 in"), WithinRel(330.2, 1e-12));
    CHECK(evaluate("45 deg") == 45.0);
    CHECK_THAT(evaluate("pi rad"), WithinRel(180.0, 1e-12));
    CHECK_THAT(evaluate("(1 + 1) in"), WithinRel(50.8, 1e-12));
    // A unit has to follow a number.
    CHECK_THROWS_WITH(evaluate("mm"), ContainsSubstring("has to follow a number"));
    CHECK_THROWS_AS(evaluate("width mm", table({{"width", 4.0}})), ExpressionError);
}

TEST_CASE("Names are looked up, and functions applied", "[core][expression]") {
    const Lookup values = table({{"width", 60.0}, {"height", 40.0}, {"wall_2", 2.0}});
    CHECK(evaluate("width / 2 + 3 mm", values) == 33.0);
    CHECK(evaluate("width - 2 * wall_2", values) == 56.0);
    CHECK(evaluate("max(width, height, 100)", values) == 100.0);
    CHECK(evaluate("min(width, height)", values) == 40.0);
    CHECK_THAT(evaluate("sqrt(width^2 + height^2)", values), WithinRel(72.11102550927978, 1e-12));
    CHECK_THAT(evaluate("sin(30)"), WithinRel(0.5, 1e-12));
    CHECK_THAT(evaluate("cos(60 deg)"), WithinRel(0.5, 1e-12));
    CHECK_THAT(evaluate("atan(1)"), WithinRel(45.0, 1e-12));
    CHECK_THAT(evaluate("asin(0.5)"), WithinRel(30.0, 1e-12));
    CHECK(evaluate("round(2.5) + floor(1.7) + ceil(1.2) + abs(-1)") == 7.0);

    CHECK_THROWS_WITH(evaluate("width + depth", values),
                      ContainsSubstring("There is no parameter called depth"));
    CHECK_THROWS_WITH(evaluate("width / 0", values), ContainsSubstring("divides by zero"));
    CHECK_THROWS_WITH(evaluate("sqrt(-1)"), ContainsSubstring("negative"));
    CHECK_THROWS_WITH(evaluate("min(1)"), ContainsSubstring("two values or more"));
    CHECK_THROWS_WITH(evaluate("sin(1, 2)"), ContainsSubstring("one value"));
    CHECK_THROWS_WITH(evaluate("asin(2)"), ContainsSubstring("from -1 to 1"));
}

TEST_CASE("Expressions that do not parse say where", "[core][expression]") {
    CHECK_THROWS_WITH(evaluate(""), ContainsSubstring("nothing to work out"));
    CHECK_THROWS_WITH(evaluate("1 +"), ContainsSubstring("missing at the end"));
    CHECK_THROWS_WITH(evaluate("(1 + 2"), ContainsSubstring("\")\" is missing"));
    CHECK_THROWS_WITH(evaluate("1 2"), ContainsSubstring("character 3"));
    CHECK_THROWS_WITH(evaluate("1.2.3"), ContainsSubstring("is not a number"));
    CHECK_THROWS_WITH(evaluate("2 # 3"), ContainsSubstring("\"#\" is not expected"));
    CHECK_THROWS_WITH(evaluate("sqrt 4"), ContainsSubstring("needs \"(\""));
    CHECK_THROWS_WITH(evaluate("10 ^ 400"), ContainsSubstring("does not come out as a number"));
}

TEST_CASE("The names in an expression, and which names are allowed", "[core][expression]") {
    CHECK(namesIn("width / 2 + sqrt(height) * width - 3 mm + pi") ==
          std::vector<std::string>{"width", "height"});
    CHECK(namesIn("42").empty());
    CHECK_THROWS_AS(namesIn("(width"), ExpressionError);

    CHECK(isValidName("width"));
    CHECK(isValidName("_wall2"));
    CHECK_FALSE(isValidName(""));
    CHECK_FALSE(isValidName("2wide"));
    CHECK_FALSE(isValidName("wall thickness"));
    CHECK_FALSE(isValidName("sqrt"));
    CHECK_FALSE(isValidName("mm"));
    CHECK_FALSE(isValidName("pi"));

    CHECK(isPlainNumber("40"));
    CHECK(isPlainNumber("-2.5"));
    CHECK(isPlainNumber("40 mm"));
    CHECK_FALSE(isPlainNumber("width"));
    CHECK_FALSE(isPlainNumber("20 + 20"));
    CHECK_FALSE(isPlainNumber("2 * 3"));
    CHECK_FALSE(isPlainNumber("("));
}

TEST_CASE("A name is changed wherever it is used, and nowhere else", "[core][expression]") {
    CHECK(renamed("width / 2 + widths + width_2 - 2 * width", "width", "span") ==
          "span / 2 + widths + width_2 - 2 * span");
    CHECK(renamed("1e3 + e", "e", "x") == "1e3 + x");
    CHECK(renamed("3 mm + sqrt(w)", "w", "wide") == "3 mm + sqrt(wide)");
}
