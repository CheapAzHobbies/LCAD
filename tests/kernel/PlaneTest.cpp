// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Plane.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/kernel/Transform.h>

#include <Tolerances.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>

using namespace lcad::kernel;
using Catch::Matchers::WithinAbs;
using lcad::test::lengthTolerance;

namespace {

const double pi = 3.14159265358979323846;

void checkPoint(const Point& actual, double x, double y, double z) {
    CHECK_THAT(actual.x, WithinAbs(x, lengthTolerance));
    CHECK_THAT(actual.y, WithinAbs(y, lengthTolerance));
    CHECK_THAT(actual.z, WithinAbs(z, lengthTolerance));
}

} // namespace

TEST_CASE("The three standard planes are seen the right way round", "[kernel][plane]") {
    // On each, u runs to the right and v up in the matching standard view.
    checkPoint(toWorld(planeXY(), 3.0, 4.0), 3.0, 4.0, 0.0);
    checkPoint(toWorld(planeXZ(), 3.0, 4.0), 3.0, 0.0, 4.0);
    checkPoint(toWorld(planeYZ(), 3.0, 4.0), 0.0, 3.0, 4.0);
    CHECK(planeXY().normal.z == 1.0);
    CHECK(planeXZ().normal.y == -1.0);
    CHECK(planeYZ().normal.x == 1.0);
}

TEST_CASE("Points go to a plane's coordinates and back", "[kernel][plane]") {
    const Plane plane = makePlane({10.0, -5.0, 2.0}, {1.0, 1.0, 0.0}, {-1.0, 1.0, 0.0});

    const Point world = toWorld(plane, 7.5, -3.0);
    const PlaneCoordinates local = toLocal(plane, world);
    CHECK_THAT(local.u, WithinAbs(7.5, lengthTolerance));
    CHECK_THAT(local.v, WithinAbs(-3.0, lengthTolerance));
    CHECK_THAT(local.w, WithinAbs(0.0, lengthTolerance));

    // A point off the plane has a height, measured along the normal.
    const double s = std::sqrt(0.5);
    const PlaneCoordinates above = toLocal(plane, {10.0 + 4.0 * s, -5.0 + 4.0 * s, 2.0});
    CHECK_THAT(above.w, WithinAbs(4.0, lengthTolerance));
    CHECK_THAT(above.u, WithinAbs(0.0, lengthTolerance));
}

TEST_CASE("A plane needs a normal and an x direction that are square", "[kernel][plane]") {
    const Point origin{0.0, 0.0, 0.0};
    const double nan = std::numeric_limits<double>::quiet_NaN();

    // Lengths do not matter: they are made unit.
    const Plane plane = makePlane(origin, {0.0, 0.0, 5.0}, {0.2, 0.0, 0.0});
    CHECK_THAT(plane.normal.z, WithinAbs(1.0, 1e-12));
    CHECK_THAT(plane.xDirection.x, WithinAbs(1.0, 1e-12));
    CHECK_THAT(yDirection(plane).y, WithinAbs(1.0, 1e-12));

    CHECK_THROWS_AS(makePlane(origin, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}), std::invalid_argument);
    CHECK_THROWS_AS(makePlane(origin, {0.0, 0.0, 1.0}, {0.0, 0.0, 0.0}), std::invalid_argument);
    CHECK_THROWS_AS(makePlane(origin, {0.0, 0.0, 1.0}, {1.0, 0.0, 1.0}), std::invalid_argument);
    CHECK_THROWS_AS(makePlane({nan, 0.0, 0.0}, {0.0, 0.0, 1.0}, {1.0, 0.0, 0.0}),
                    std::invalid_argument);
}

TEST_CASE("Curves on a plane have the lengths and places they should", "[kernel][plane]") {
    const Plane plane = planeXZ();

    SECTION("a segment") {
        const Shape segment = makeSegment({0.0, 0.0, 0.0}, {3.0, 0.0, 4.0});
        CHECK(isValid(segment));
        CHECK_THAT(length(segment), WithinAbs(5.0, lengthTolerance));
        CHECK(countTopology(segment).edges == 1);
        CHECK_THROWS_AS(makeSegment({1.0, 1.0, 1.0}, {1.0, 1.0, 1.0}), std::invalid_argument);
    }
    SECTION("a circle") {
        const Shape circle = makeCircleEdge(plane, 10.0, 20.0, 5.0);
        CHECK(isValid(circle));
        CHECK_THAT(length(circle), WithinAbs(10.0 * pi, lengthTolerance));
        const auto box = boundingBox(circle);
        REQUIRE(box.has_value());
        // In the XZ plane: flat in Y, around X = 10 and Z = 20.
        CHECK_THAT(box->min.x, WithinAbs(5.0, lengthTolerance));
        CHECK_THAT(box->max.x, WithinAbs(15.0, lengthTolerance));
        CHECK_THAT(box->min.y, WithinAbs(0.0, lengthTolerance));
        CHECK_THAT(box->max.y, WithinAbs(0.0, lengthTolerance));
        CHECK_THAT(box->max.z, WithinAbs(25.0, lengthTolerance));
        CHECK_THROWS_AS(makeCircleEdge(plane, 0.0, 0.0, 0.0), std::invalid_argument);
    }
    SECTION("an arc runs counter-clockwise from its start") {
        // A quarter, from the direction of u to the direction of v.
        const Shape quarter = makeArcEdge(planeXY(), 0.0, 0.0, 4.0, 0.0, 0.0, 4.0);
        CHECK(isValid(quarter));
        CHECK_THAT(length(quarter), WithinAbs(2.0 * pi, lengthTolerance));
        const auto box = boundingBox(quarter);
        CHECK_THAT(box->min.x, WithinAbs(0.0, lengthTolerance));
        CHECK_THAT(box->min.y, WithinAbs(0.0, lengthTolerance));
        // The other way round the same two points is the remaining three quarters.
        const Shape rest = makeArcEdge(planeXY(), 0.0, 0.0, 0.0, 4.0, 4.0, 0.0);
        CHECK_THAT(length(rest), WithinAbs(6.0 * pi, lengthTolerance));
        CHECK_THAT(boundingBox(rest)->min.x, WithinAbs(-4.0, lengthTolerance));
        CHECK_THROWS_AS(makeArcEdge(planeXY(), 0.0, 0.0, 0.0, 0.0, 1.0, 0.0),
                        std::invalid_argument);
    }
    SECTION("a vertex and a compound") {
        const Shape vertex = makeVertex({1.0, 2.0, 3.0});
        CHECK(countTopology(vertex).vertices == 1);
        const Shape both = makeCompound({makeSegment({0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}), Shape(),
                                         makeSegment({0.0, 0.0, 0.0}, {0.0, 2.0, 0.0})});
        CHECK(countTopology(both).edges == 2);
        CHECK_THAT(length(both), WithinAbs(3.0, lengthTolerance));
        CHECK(makeCompound({}).isNull());
        CHECK(makeCompound({Shape()}).isNull());
    }
}

TEST_CASE("A flat face gives its plane, facing out of the solid", "[kernel][plane]") {
    const Shape box = translated(makeBox(100.0, 60.0, 40.0), 10.0, 20.0, 30.0);
    int found = 0;
    for (int i = 0; i < subShapeCount(box, SubShapeKind::Face); ++i) {
        const Shape face = subShape(box, SubShapeKind::Face, i);
        const auto plane = planeOfFace(face);
        REQUIRE(plane.has_value());
        const auto bounds = boundingBox(face);
        const Point middle{(bounds->min.x + bounds->max.x) / 2.0,
                           (bounds->min.y + bounds->max.y) / 2.0,
                           (bounds->min.z + bounds->max.z) / 2.0};
        // The face lies in its plane.
        CHECK_THAT(toLocal(*plane, middle).w, WithinAbs(0.0, lengthTolerance));
        // The normal points away from the middle of the box.
        const Point center{60.0, 50.0, 50.0};
        CHECK(toLocal(*plane, center).w < -1.0);
        // The plane's origin is the model's origin dropped onto it.
        const PlaneCoordinates modelOrigin = toLocal(*plane, {0.0, 0.0, 0.0});
        CHECK_THAT(modelOrigin.u, WithinAbs(0.0, lengthTolerance));
        CHECK_THAT(modelOrigin.v, WithinAbs(0.0, lengthTolerance));
        if (plane->normal.z > 0.99) {
            // The top: at Z = 70, with u along X.
            CHECK_THAT(plane->origin.z, WithinAbs(70.0, lengthTolerance));
            CHECK_THAT(plane->xDirection.x, WithinAbs(1.0, 1e-9));
            ++found;
        }
        if (plane->normal.x > 0.99) {
            // A side facing along X cannot have u along X; it takes Y.
            CHECK_THAT(plane->xDirection.y, WithinAbs(1.0, 1e-9));
            ++found;
        }
    }
    CHECK(found == 2);

    SECTION("what is not one flat face has no plane") {
        const Shape cylinder = makeCylinder(5.0, 10.0);
        int flat = 0;
        for (int i = 0; i < subShapeCount(cylinder, SubShapeKind::Face); ++i) {
            flat += planeOfFace(subShape(cylinder, SubShapeKind::Face, i)).has_value() ? 1 : 0;
        }
        // The two ends, not the side.
        CHECK(flat == 2);
        CHECK_FALSE(planeOfFace(box).has_value());
        CHECK_FALSE(planeOfFace(Shape()).has_value());
        CHECK_FALSE(planeOfFace(subShape(box, SubShapeKind::Edge, 0)).has_value());
    }
}

TEST_CASE("Edges and vertices drop onto a plane as lines, circles, arcs, and points",
          "[kernel][plane]") {
    // A box from (10, 20, 0) to (110, 80, 40), and the XY plane under it.
    const Shape box = translated(makeBox(100.0, 60.0, 40.0), 10.0, 20.0, 0.0);
    const Plane floor = planeXY();

    SECTION("straight edges: along the plane as lines, square to it as points") {
        int flat = 0;
        int upright = 0;
        for (int i = 0; i < subShapeCount(box, SubShapeKind::Edge); ++i) {
            const auto curve = projectOnto(floor, subShape(box, SubShapeKind::Edge, i));
            REQUIRE(curve.has_value());
            if (curve->kind == PlaneCurve::Kind::Point) {
                ++upright;
                // One of the four corners of the footprint.
                CHECK((std::abs(curve->u1 - 10.0) < 1e-9 || std::abs(curve->u1 - 110.0) < 1e-9));
                CHECK((std::abs(curve->v1 - 20.0) < 1e-9 || std::abs(curve->v1 - 80.0) < 1e-9));
            } else {
                REQUIRE(curve->kind == PlaneCurve::Kind::Segment);
                ++flat;
                const double length = std::hypot(curve->u2 - curve->u1, curve->v2 - curve->v1);
                CHECK((std::abs(length - 100.0) < 1e-9 || std::abs(length - 60.0) < 1e-9));
            }
        }
        // Eight edges lie flat (four at the bottom, four at the top), four stand upright.
        CHECK(flat == 8);
        CHECK(upright == 4);
    }

    SECTION("a vertex is a point, at its place on the plane") {
        // On the XZ plane, u is X and v is Z.
        bool found = false;
        for (int i = 0; i < subShapeCount(box, SubShapeKind::Vertex); ++i) {
            const auto point = projectOnto(planeXZ(), subShape(box, SubShapeKind::Vertex, i));
            REQUIRE(point.has_value());
            REQUIRE(point->kind == PlaneCurve::Kind::Point);
            found =
                found || (std::abs(point->u1 - 110.0) < 1e-9 && std::abs(point->v1 - 40.0) < 1e-9);
        }
        CHECK(found);
    }

    SECTION("a circle parallel to the plane is a circle; at a slant it is nothing") {
        // A cylinder standing on the XY plane at (30, 40): radius 12, height 25.
        const Shape cylinder = translated(makeCylinder(12.0, 25.0), 30.0, 40.0, 0.0);
        int circles = 0;
        int seams = 0;
        for (int i = 0; i < subShapeCount(cylinder, SubShapeKind::Edge); ++i) {
            const Shape edge = subShape(cylinder, SubShapeKind::Edge, i);
            const auto onFloor = projectOnto(floor, edge);
            REQUIRE(onFloor.has_value());
            if (onFloor->kind == PlaneCurve::Kind::Circle) {
                ++circles;
                CHECK_THAT(onFloor->u1, WithinAbs(30.0, 1e-9));
                CHECK_THAT(onFloor->v1, WithinAbs(40.0, 1e-9));
                CHECK_THAT(onFloor->radius, WithinAbs(12.0, 1e-9));
                // Seen from the front the same circle is an ellipse squashed to a line.
                CHECK_FALSE(projectOnto(planeXZ(), edge).has_value());
            } else {
                // The seam down the side: upright, so a point on the circle.
                CHECK(onFloor->kind == PlaneCurve::Kind::Point);
                CHECK_THAT(std::hypot(onFloor->u1 - 30.0, onFloor->v1 - 40.0),
                           WithinAbs(12.0, 1e-9));
                ++seams;
            }
        }
        CHECK(circles == 2);
        CHECK(seams == 1);
    }

    SECTION("part of a circle is an arc, the same part from whichever side it is seen") {
        // A quarter of a circle of radius 20 around (5, 5) at height 3, from angle 0 to 90
        // degrees: from (25, 5) to (5, 25).
        Plane high = planeXY();
        high.origin = {0.0, 0.0, 3.0};
        const Shape quarter = makeArcEdge(high, 5.0, 5.0, 25.0, 5.0, 5.0, 25.0);
        CHECK_THAT(length(quarter), WithinAbs(M_PI * 10.0, 1e-9));

        const auto fromAbove = projectOnto(floor, quarter);
        REQUIRE(fromAbove.has_value());
        REQUIRE(fromAbove->kind == PlaneCurve::Kind::Arc);
        CHECK_THAT(fromAbove->u1, WithinAbs(5.0, 1e-9));
        CHECK_THAT(fromAbove->v1, WithinAbs(5.0, 1e-9));
        CHECK_THAT(fromAbove->u2, WithinAbs(25.0, 1e-9));
        CHECK_THAT(fromAbove->v2, WithinAbs(5.0, 1e-9));
        CHECK_THAT(fromAbove->u3, WithinAbs(5.0, 1e-9));
        CHECK_THAT(fromAbove->v3, WithinAbs(25.0, 1e-9));
        // Built again from what was projected, it is the same quarter, not the other three.
        CHECK_THAT(length(makeArcEdge(floor, fromAbove->u1, fromAbove->v1, fromAbove->u2,
                                      fromAbove->v2, fromAbove->u3, fromAbove->v3)),
                   WithinAbs(M_PI * 10.0, 1e-9));

        // From below, left and right are swapped, and the arc still has to be that quarter.
        const Plane below = makePlane({0.0, 0.0, 0.0}, {0.0, 0.0, -1.0}, {1.0, 0.0, 0.0});
        const auto fromBelow = projectOnto(below, quarter);
        REQUIRE(fromBelow.has_value());
        REQUIRE(fromBelow->kind == PlaneCurve::Kind::Arc);
        const Shape rebuilt = makeArcEdge(below, fromBelow->u1, fromBelow->v1, fromBelow->u2,
                                          fromBelow->v2, fromBelow->u3, fromBelow->v3);
        CHECK_THAT(length(rebuilt), WithinAbs(M_PI * 10.0, 1e-9));
        // Its middle is under the middle of the original: (5 + 20 cos 45, 5 + 20 sin 45).
        const auto box2 = boundingBox(rebuilt);
        REQUIRE(box2.has_value());
        CHECK_THAT(box2->max.x, WithinAbs(25.0, 1e-6));
        CHECK_THAT(box2->max.y, WithinAbs(25.0, 1e-6));
        CHECK_THAT(box2->min.x, WithinAbs(5.0, 1e-6));
    }

    SECTION("anything else is not projected") {
        CHECK_FALSE(projectOnto(floor, Shape()).has_value());
        CHECK_FALSE(projectOnto(floor, box).has_value());
        CHECK_FALSE(projectOnto(floor, subShape(box, SubShapeKind::Face, 0)).has_value());
    }
}
