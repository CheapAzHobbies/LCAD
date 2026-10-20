// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/kernel/Transform.h>
#include <lcad/kernel/occt/ShapeAccess.h>

#include <Tolerances.h>

#include <BRepBuilderAPI_MakeFace.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pln.hxx>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <stdexcept>

using namespace lcad::kernel;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using lcad::test::exactPropertyTolerance;
using lcad::test::lengthTolerance;

TEST_CASE("Measuring a null shape reports it as empty", "[kernel][measure]") {
    const Shape nothing;

    CHECK(volume(nothing) == 0.0);
    CHECK(surfaceArea(nothing) == 0.0);
    CHECK_FALSE(centerOfMass(nothing).has_value());
    CHECK_FALSE(boundingBox(nothing).has_value());
    CHECK_FALSE(isValid(nothing));

    const TopologyCount count = countTopology(nothing);
    CHECK(count.solids == 0);
    CHECK(count.faces == 0);
    CHECK(count.edges == 0);
    CHECK(count.vertices == 0);
}

TEST_CASE("A lone face has area but encloses no volume", "[kernel][measure]") {
    // A 4 mm by 2.5 mm rectangle in the XY plane.
    const Shape face = occt::wrap(BRepBuilderAPI_MakeFace(gp_Pln(), 0.0, 4.0, 0.0, 2.5).Face());
    REQUIRE(isValid(face));

    CHECK_THAT(surfaceArea(face), WithinRel(10.0, exactPropertyTolerance));
    CHECK(volume(face) == 0.0);
    CHECK_FALSE(centerOfMass(face).has_value());

    const TopologyCount count = countTopology(face);
    CHECK(count.solids == 0);
    CHECK(count.shells == 0);
    CHECK(count.faces == 1);
    CHECK(count.edges == 4);
    CHECK(count.vertices == 4);
}

TEST_CASE("Two shapes are measured against each other", "[kernel][measure]") {
    // A box 50 by 30 by 20 from the origin, and a rod of radius 5 standing 30 beyond it in X.
    const Shape box = makeBox(50.0, 30.0, 20.0);
    const Shape rod = translated(makeCylinder(5.0, 20.0), 85.0, 15.0, 0.0);

    // The faces of the box at x = 0 and x = 50, its bottom, and its edge along X at the origin.
    Shape left;
    Shape right;
    Shape bottom;
    for (int i = 0; i < subShapeCount(box, SubShapeKind::Face); ++i) {
        const Shape face = subShape(box, SubShapeKind::Face, i);
        const auto bounds = boundingBox(face);
        REQUIRE(bounds.has_value());
        if (bounds->max.x < 1e-9) {
            left = face;
        } else if (bounds->min.x > 50.0 - 1e-9) {
            right = face;
        } else if (bounds->max.z < 1e-9) {
            bottom = face;
        }
    }
    REQUIRE_FALSE(left.isNull());
    REQUIRE_FALSE(right.isNull());
    REQUIRE_FALSE(bottom.isNull());

    // Opposite sides: 50 apart, and parallel.
    const Measurement across = measureBetween(left, right);
    CHECK_THAT(across.distance, WithinAbs(50.0, 1e-9));
    REQUIRE(across.angle.has_value());
    CHECK_THAT(*across.angle, WithinAbs(0.0, 1e-9));
    CHECK_THAT(across.to.x - across.from.x, WithinAbs(50.0, 1e-9));

    // A side and the bottom meet, square.
    const Measurement corner = measureBetween(left, bottom);
    CHECK_THAT(corner.distance, WithinAbs(0.0, 1e-9));
    REQUIRE(corner.angle.has_value());
    CHECK_THAT(*corner.angle, WithinAbs(90.0, 1e-9));

    // The rod is 30 from the right side, to its surface: 85 - 5 - 50.
    const Measurement gap = measureBetween(right, rod);
    CHECK_THAT(gap.distance, WithinAbs(30.0, 1e-6));
    CHECK_FALSE(gap.angle.has_value());
    // Whole bodies, too.
    CHECK_THAT(measureBetween(box, rod).distance, WithinAbs(30.0, 1e-6));

    // A straight edge lies in the bottom: no angle between them. A vertical edge is square to it.
    int checked = 0;
    for (int i = 0; i < subShapeCount(box, SubShapeKind::Edge); ++i) {
        const Shape edge = subShape(box, SubShapeKind::Edge, i);
        const auto bounds = boundingBox(edge);
        const Measurement m = measureBetween(edge, bottom);
        REQUIRE(m.angle.has_value());
        if (bounds->max.z < 1e-9) {
            CHECK_THAT(*m.angle, WithinAbs(0.0, 1e-9));
            ++checked;
        } else if (bounds->max.z - bounds->min.z > 19.0) {
            CHECK_THAT(*m.angle, WithinAbs(90.0, 1e-9));
            ++checked;
        }
    }
    CHECK(checked == 8);

    // Round things have a radius; flat ones do not.
    int round = 0;
    for (int i = 0; i < subShapeCount(rod, SubShapeKind::Face); ++i) {
        if (const auto radius = radiusOf(subShape(rod, SubShapeKind::Face, i))) {
            CHECK_THAT(*radius, WithinAbs(5.0, 1e-9));
            ++round;
        }
    }
    CHECK(round == 1);
    CHECK_FALSE(radiusOf(left).has_value());
    CHECK_FALSE(radiusOf(Shape()).has_value());
    CHECK_THROWS_AS(measureBetween(Shape(), box), std::invalid_argument);
}

TEST_CASE("A solid's mass properties follow from its density", "[kernel][measure]") {
    // 10 x 20 x 30 mm of aluminum, 2.70 g/cm3: 6 cm3, 16.2 g.
    const Shape box = makeBox(10.0, 20.0, 30.0);
    const auto properties = massProperties(box, 2.70);
    REQUIRE(properties.has_value());
    CHECK_THAT(properties->volume, WithinRel(6000.0, exactPropertyTolerance));
    CHECK_THAT(properties->mass, WithinRel(16.2, exactPropertyTolerance));
    CHECK_THAT(properties->center.x, WithinAbs(5.0, lengthTolerance));
    CHECK_THAT(properties->center.y, WithinAbs(10.0, lengthTolerance));
    CHECK_THAT(properties->center.z, WithinAbs(15.0, lengthTolerance));
    // About its center, a box of sides a, b, c has m (b^2 + c^2) / 12 about the first axis.
    const double m = 16.2;
    CHECK_THAT(properties->inertia[0][0], WithinRel(m * (400.0 + 900.0) / 12.0, 1e-9));
    CHECK_THAT(properties->inertia[1][1], WithinRel(m * (100.0 + 900.0) / 12.0, 1e-9));
    CHECK_THAT(properties->inertia[2][2], WithinRel(m * (100.0 + 400.0) / 12.0, 1e-9));
    CHECK_THAT(properties->inertia[0][1], WithinAbs(0.0, 1e-6));

    const PrincipalInertia principal = principalInertia(*properties);
    CHECK_THAT(principal.moments[0], WithinRel(m * 500.0 / 12.0, 1e-9));
    CHECK_THAT(principal.moments[2], WithinRel(m * 1300.0 / 12.0, 1e-9));
    // The axis it turns most easily about is the long one, Z.
    CHECK_THAT(std::abs(principal.axes[0].z), WithinAbs(1.0, 1e-9));

    CHECK_FALSE(massProperties(Shape(), 1.0).has_value());
    CHECK_THROWS_AS(massProperties(box, 0.0), std::invalid_argument);
}

TEST_CASE("Masses taken together are one mass", "[kernel][measure]") {
    // Two halves of a 20 x 20 x 20 box give what the whole box gives.
    const auto left = massProperties(makeBox(10.0, 20.0, 20.0), 1.0);
    const auto right = massProperties(translated(makeBox(10.0, 20.0, 20.0), 10.0, 0.0, 0.0), 1.0);
    const auto whole = massProperties(makeBox(20.0, 20.0, 20.0), 1.0);
    REQUIRE(left.has_value());
    REQUIRE(right.has_value());
    REQUIRE(whole.has_value());

    const MassProperties both = combined({*left, *right});
    CHECK_THAT(both.mass, WithinRel(whole->mass, 1e-12));
    CHECK_THAT(both.volume, WithinRel(whole->volume, 1e-12));
    CHECK_THAT(both.center.x, WithinAbs(10.0, lengthTolerance));
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            CHECK_THAT(both.inertia[row][column],
                       WithinAbs(whole->inertia[row][column], 1e-6 * whole->inertia[0][0]));
        }
    }
    CHECK(combined({}).mass == 0.0);

    SECTION("a tilted rod's principal axis lies along it") {
        // A rod along X and Y at once: its products of inertia are not zero.
        const Shape rod = makeBox(100.0, 4.0, 4.0);
        const Placement tilt = rotation(Axis{{0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}}, 0.5);
        const auto tilted = massProperties(placed(rod, tilt), 1.0);
        REQUIRE(tilted.has_value());
        CHECK(std::abs(tilted->inertia[0][1]) > 1.0);
        const PrincipalInertia principal = principalInertia(*tilted);
        CHECK_THAT(std::abs(principal.axes[0].x), WithinAbs(std::cos(0.5), 1e-9));
        CHECK_THAT(std::abs(principal.axes[0].y), WithinAbs(std::sin(0.5), 1e-9));
    }
}

TEST_CASE("Solids that share space overlap; solids that touch do not", "[kernel][measure]") {
    const Shape box = makeBox(10.0, 10.0, 10.0);

    SECTION("overlapping by 2 mm") {
        const auto overlap = overlapOf(box, translated(box, 8.0, 0.0, 0.0));
        REQUIRE(overlap.has_value());
        CHECK_THAT(overlap->volume, WithinRel(200.0, exactPropertyTolerance));
        const auto bounds = boundingBox(overlap->shape);
        REQUIRE(bounds.has_value());
        CHECK_THAT(bounds->min.x, WithinAbs(8.0, lengthTolerance));
        CHECK_THAT(bounds->max.x, WithinAbs(10.0, lengthTolerance));
    }
    SECTION("face to face") {
        CHECK_FALSE(overlapOf(box, translated(box, 10.0, 0.0, 0.0)).has_value());
    }
    SECTION("far apart") {
        CHECK_FALSE(overlapOf(box, translated(box, 50.0, 0.0, 0.0)).has_value());
    }
    SECTION("a pin in a hole that is too small") {
        const Shape pin = translated(makeCylinder(3.0, 20.0), 5.0, 5.0, -5.0);
        const auto overlap = overlapOf(box, pin);
        REQUIRE(overlap.has_value());
        CHECK_THAT(overlap->volume, WithinRel(3.14159265358979323846 * 9.0 * 10.0, 1e-9));
    }
    CHECK_FALSE(overlapOf(box, Shape()).has_value());
}

TEST_CASE("The kind of a face, an edge, or a vertex is told", "[kernel][measure]") {
    const Shape cylinder = makeCylinder(5.0, 10.0);
    int flat = 0;
    int round = 0;
    for (int i = 0; i < subShapeCount(cylinder, SubShapeKind::Face); ++i) {
        const PartKind kind = partKind(subShape(cylinder, SubShapeKind::Face, i));
        flat += kind == PartKind::Flat ? 1 : 0;
        round += kind == PartKind::Cylindrical ? 1 : 0;
    }
    CHECK(flat == 2);
    CHECK(round == 1);
    CHECK(partKind(subShape(makeSphere(3.0), SubShapeKind::Face, 0)) == PartKind::Spherical);
    CHECK(partKind(subShape(makeBox(1.0, 1.0, 1.0), SubShapeKind::Edge, 0)) == PartKind::Straight);
    CHECK(partKind(subShape(makeBox(1.0, 1.0, 1.0), SubShapeKind::Vertex, 0)) == PartKind::Point);
    CHECK(partKind(Shape()) == PartKind::Other);
    CHECK(partKind(cylinder) == PartKind::Other);
}
