// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/Transform.h>

#include <Tolerances.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <limits>
#include <stdexcept>

using namespace lcad::kernel;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using lcad::test::exactPropertyTolerance;
using lcad::test::lengthTolerance;

TEST_CASE("translated moves a shape without changing it", "[kernel][transform]") {
    const Shape box = makeBox(10.0, 20.0, 30.0);

    const Shape moved = translated(box, 100.0, -50.0, 7.5);

    REQUIRE(isValid(moved));
    CHECK_THAT(volume(moved), WithinRel(6000.0, exactPropertyTolerance));
    const auto bounds = boundingBox(moved);
    REQUIRE(bounds.has_value());
    CHECK_THAT(bounds->min.x, WithinAbs(100.0, lengthTolerance));
    CHECK_THAT(bounds->min.y, WithinAbs(-50.0, lengthTolerance));
    CHECK_THAT(bounds->min.z, WithinAbs(7.5, lengthTolerance));
    CHECK_THAT(bounds->max.x, WithinAbs(110.0, lengthTolerance));
    CHECK_THAT(bounds->max.y, WithinAbs(-30.0, lengthTolerance));
    CHECK_THAT(bounds->max.z, WithinAbs(37.5, lengthTolerance));

    SECTION("the original stays where it was") {
        const auto original = boundingBox(box);
        REQUIRE(original.has_value());
        CHECK_THAT(original->min.x, WithinAbs(0.0, lengthTolerance));
        CHECK_THAT(original->max.z, WithinAbs(30.0, lengthTolerance));
    }
}

TEST_CASE("translated accepts a null shape and rejects distances that are not finite",
          "[kernel][transform]") {
    CHECK(translated(Shape(), 1.0, 2.0, 3.0).isNull());

    const Shape box = makeBox(1.0, 1.0, 1.0);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    CHECK_THROWS_AS(translated(box, nan, 0.0, 0.0), std::invalid_argument);
    CHECK_THROWS_AS(translated(box, 0.0, infinity, 0.0), std::invalid_argument);
    CHECK_THROWS_AS(translated(box, 0.0, 0.0, -infinity), std::invalid_argument);
}
