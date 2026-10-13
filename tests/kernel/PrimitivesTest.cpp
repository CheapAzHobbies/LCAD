// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Primitives.h>

#include <Tolerances.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>

using namespace lcad::kernel;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using lcad::test::curvedPropertyTolerance;
using lcad::test::exactPropertyTolerance;
using lcad::test::lengthTolerance;

namespace {

const double pi = 3.14159265358979323846;

void checkBounds(const Shape& shape, const Point& min, const Point& max) {
    const auto bounds = boundingBox(shape);
    REQUIRE(bounds.has_value());
    CHECK_THAT(bounds->min.x, WithinAbs(min.x, lengthTolerance));
    CHECK_THAT(bounds->min.y, WithinAbs(min.y, lengthTolerance));
    CHECK_THAT(bounds->min.z, WithinAbs(min.z, lengthTolerance));
    CHECK_THAT(bounds->max.x, WithinAbs(max.x, lengthTolerance));
    CHECK_THAT(bounds->max.y, WithinAbs(max.y, lengthTolerance));
    CHECK_THAT(bounds->max.z, WithinAbs(max.z, lengthTolerance));
}

void checkCenter(const Shape& shape, const Point& expected) {
    const auto center = centerOfMass(shape);
    REQUIRE(center.has_value());
    CHECK_THAT(center->x, WithinAbs(expected.x, lengthTolerance));
    CHECK_THAT(center->y, WithinAbs(expected.y, lengthTolerance));
    CHECK_THAT(center->z, WithinAbs(expected.z, lengthTolerance));
}

} // namespace

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

    SECTION("its center of mass is the center of the box") {
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

TEST_CASE("makeCylinder builds a valid cylinder standing on the XY plane", "[kernel][primitives]") {
    const double r = 5.0;
    const double h = 12.0;
    const Shape cylinder = makeCylinder(r, h);

    REQUIRE_FALSE(cylinder.isNull());
    CHECK(isValid(cylinder));

    const TopologyCount count = countTopology(cylinder);
    CHECK(count.solids == 1);
    CHECK(count.shells == 1);
    // The side and the two ends.
    CHECK(count.faces == 3);

    CHECK_THAT(volume(cylinder), WithinRel(pi * r * r * h, curvedPropertyTolerance));
    CHECK_THAT(surfaceArea(cylinder),
               WithinRel(2.0 * pi * r * h + 2.0 * pi * r * r, curvedPropertyTolerance));
    checkBounds(cylinder, {-r, -r, 0.0}, {r, r, h});
    checkCenter(cylinder, {0.0, 0.0, h / 2.0});
}

TEST_CASE("makeSphere builds a valid sphere centered on the origin", "[kernel][primitives]") {
    const double r = 7.0;
    const Shape sphere = makeSphere(r);

    REQUIRE_FALSE(sphere.isNull());
    CHECK(isValid(sphere));

    const TopologyCount count = countTopology(sphere);
    CHECK(count.solids == 1);
    CHECK(count.shells == 1);
    CHECK(count.faces == 1);

    CHECK_THAT(volume(sphere), WithinRel(4.0 / 3.0 * pi * r * r * r, curvedPropertyTolerance));
    CHECK_THAT(surfaceArea(sphere), WithinRel(4.0 * pi * r * r, curvedPropertyTolerance));
    checkBounds(sphere, {-r, -r, -r}, {r, r, r});
    checkCenter(sphere, {0.0, 0.0, 0.0});
}

TEST_CASE("makeCone builds pointed and truncated cones", "[kernel][primitives]") {
    const double h = 9.0;

    SECTION("truncated: a different radius at each end") {
        const double r1 = 6.0;
        const double r2 = 2.0;
        const Shape cone = makeCone(r1, r2, h);

        REQUIRE_FALSE(cone.isNull());
        CHECK(isValid(cone));
        const TopologyCount count = countTopology(cone);
        CHECK(count.solids == 1);
        CHECK(count.faces == 3);

        const double slant = std::sqrt((r1 - r2) * (r1 - r2) + h * h);
        CHECK_THAT(volume(cone), WithinRel(pi * h / 3.0 * (r1 * r1 + r1 * r2 + r2 * r2),
                                           curvedPropertyTolerance));
        CHECK_THAT(surfaceArea(cone),
                   WithinRel(pi * (r1 + r2) * slant + pi * r1 * r1 + pi * r2 * r2,
                             curvedPropertyTolerance));
        checkBounds(cone, {-r1, -r1, 0.0}, {r1, r1, h});
    }

    SECTION("pointed: the top radius is zero") {
        const double r = 6.0;
        const Shape cone = makeCone(r, 0.0, h);

        REQUIRE_FALSE(cone.isNull());
        CHECK(isValid(cone));
        // The side and the base; the point is not a face.
        CHECK(countTopology(cone).faces == 2);
        CHECK_THAT(volume(cone), WithinRel(pi * r * r * h / 3.0, curvedPropertyTolerance));
        // The centroid of a cone is a quarter of the way up from its base.
        checkCenter(cone, {0.0, 0.0, h / 4.0});
    }

    SECTION("pointed downward: the base radius is zero") {
        const Shape cone = makeCone(0.0, 6.0, h);

        CHECK(isValid(cone));
        CHECK_THAT(volume(cone), WithinRel(pi * 36.0 * h / 3.0, curvedPropertyTolerance));
        checkCenter(cone, {0.0, 0.0, h * 3.0 / 4.0});
    }
}

TEST_CASE("makeTorus builds a valid ring in the XY plane", "[kernel][primitives]") {
    const double major = 10.0;
    const double minor = 3.0;
    const Shape torus = makeTorus(major, minor);

    REQUIRE_FALSE(torus.isNull());
    CHECK(isValid(torus));

    const TopologyCount count = countTopology(torus);
    CHECK(count.solids == 1);
    CHECK(count.shells == 1);
    CHECK(count.faces == 1);

    CHECK_THAT(volume(torus),
               WithinRel(2.0 * pi * pi * major * minor * minor, curvedPropertyTolerance));
    CHECK_THAT(surfaceArea(torus),
               WithinRel(4.0 * pi * pi * major * minor, curvedPropertyTolerance));
    checkBounds(torus, {-(major + minor), -(major + minor), -minor},
                {major + minor, major + minor, minor});
    checkCenter(torus, {0.0, 0.0, 0.0});
}

TEST_CASE("Curved primitives reject sizes that are not usable lengths", "[kernel][primitives]") {
    const double unusable =
        GENERATE(0.0, -5.0, minimumLength, std::numeric_limits<double>::quiet_NaN(),
                 std::numeric_limits<double>::infinity());
    CAPTURE(unusable);

    CHECK_THROWS_AS(makeCylinder(unusable, 1.0), std::invalid_argument);
    CHECK_THROWS_AS(makeCylinder(1.0, unusable), std::invalid_argument);
    CHECK_THROWS_AS(makeSphere(unusable), std::invalid_argument);
    CHECK_THROWS_AS(makeCone(2.0, 1.0, unusable), std::invalid_argument);
    CHECK_THROWS_AS(makeTorus(unusable, 1.0), std::invalid_argument);
    CHECK_THROWS_AS(makeTorus(10.0, unusable), std::invalid_argument);
    if (unusable != 0.0) {
        // Zero is allowed for one end of a cone, and tested above.
        CHECK_THROWS_AS(makeCone(unusable, 1.0, 1.0), std::invalid_argument);
        CHECK_THROWS_AS(makeCone(1.0, unusable, 1.0), std::invalid_argument);
    }
}

TEST_CASE("Cones and rings reject combinations that describe another shape",
          "[kernel][primitives]") {
    // Equal radii are a cylinder, and two zeros are a line.
    CHECK_THROWS_AS(makeCone(3.0, 3.0, 5.0), std::invalid_argument);
    CHECK_THROWS_AS(makeCone(0.0, 0.0, 5.0), std::invalid_argument);
    // A tube as wide as the ring, or wider, closes the hole and intersects itself.
    CHECK_THROWS_AS(makeTorus(3.0, 3.0), std::invalid_argument);
    CHECK_THROWS_AS(makeTorus(3.0, 5.0), std::invalid_argument);
}
