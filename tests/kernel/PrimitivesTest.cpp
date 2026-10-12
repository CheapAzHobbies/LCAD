#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Primitives.h>

#include <Tolerances.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <limits>
#include <stdexcept>

using namespace lcad::kernel;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using lcad::test::exactPropertyTolerance;
using lcad::test::lengthTolerance;

TEST_CASE("makeBox builds a valid solid of the requested size", "[kernel][primitives]") {
    const Shape box = makeBox(10.0, 20.0, 30.0);

    REQUIRE_FALSE(box.isNull());
    CHECK(isValid(box));

    SECTION("its topology is that of a cuboid") {
        const TopologyCount count = countTopology(box);
        CHECK(count.solids == 1);
        CHECK(count.shells == 1);
        CHECK(count.faces == 6);
        CHECK(count.wires == 6);
        CHECK(count.edges == 12);
        CHECK(count.vertices == 8);
    }

    SECTION("its volume and area match the closed-form values") {
        CHECK_THAT(volume(box), WithinRel(10.0 * 20.0 * 30.0, exactPropertyTolerance));
        CHECK_THAT(surfaceArea(box), WithinRel(2.0 * (10.0 * 20.0 + 20.0 * 30.0 + 10.0 * 30.0),
                                               exactPropertyTolerance));
    }

    SECTION("it has one corner at the origin and extends along the positive axes") {
        const auto bounds = boundingBox(box);
        REQUIRE(bounds.has_value());
        CHECK_THAT(bounds->min.x, WithinAbs(0.0, lengthTolerance));
        CHECK_THAT(bounds->min.y, WithinAbs(0.0, lengthTolerance));
        CHECK_THAT(bounds->min.z, WithinAbs(0.0, lengthTolerance));
        CHECK_THAT(bounds->max.x, WithinAbs(10.0, lengthTolerance));
        CHECK_THAT(bounds->max.y, WithinAbs(20.0, lengthTolerance));
        CHECK_THAT(bounds->max.z, WithinAbs(30.0, lengthTolerance));
    }

    SECTION("its centre of mass is the centre of the box") {
        const auto center = centerOfMass(box);
        REQUIRE(center.has_value());
        CHECK_THAT(center->x, WithinAbs(5.0, lengthTolerance));
        CHECK_THAT(center->y, WithinAbs(10.0, lengthTolerance));
        CHECK_THAT(center->z, WithinAbs(15.0, lengthTolerance));
    }
}

TEST_CASE("makeBox rejects sizes that are not usable lengths", "[kernel][primitives]") {
    const double unusable =
        GENERATE(0.0, -5.0, minimumLength, std::numeric_limits<double>::quiet_NaN(),
                 std::numeric_limits<double>::infinity());
    CAPTURE(unusable);

    CHECK_THROWS_AS(makeBox(unusable, 1.0, 1.0), std::invalid_argument);
    CHECK_THROWS_AS(makeBox(1.0, unusable, 1.0), std::invalid_argument);
    CHECK_THROWS_AS(makeBox(1.0, 1.0, unusable), std::invalid_argument);
}
