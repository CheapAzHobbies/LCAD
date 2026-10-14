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

#include <stdexcept>

using namespace lcad::kernel;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using lcad::test::exactPropertyTolerance;

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
