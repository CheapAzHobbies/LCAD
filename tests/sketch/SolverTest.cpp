// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/sketch/Sketch.h>

#include <Tolerances.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

using namespace lcad::sketch;
using Catch::Matchers::WithinAbs;
using lcad::test::lengthTolerance;

namespace {

const double pi = 3.14159265358979323846;
// Angles and the unit-less measures of parallel and square, which the solver brings to within
// 1e-9 of zero.
const double angleTolerance = 1e-8;

struct Rectangle {
    EntityId bottom, right, top, left;
    ConstraintId width = 0;
    ConstraintId height = 0;
};

// A rectangle the way the rectangle tool makes one: four lines joined at their ends, two of them
// horizontal and two vertical. Drawn a little crooked, so that there is something to solve.
Rectangle addRectangle(Sketch& sketch) {
    Rectangle r;
    r.bottom = sketch.addLine({1.0, 2.0}, {38.0, 2.5});
    r.right = sketch.addLine({38.5, 2.0}, {39.0, 21.0});
    r.top = sketch.addLine({39.0, 22.0}, {0.5, 21.0});
    r.left = sketch.addLine({1.5, 21.5}, {1.0, 2.5});
    const auto join = [&sketch](EntityId first, EntityId second) {
        sketch.addConstraint(ConstraintKind::Coincident, sketch.find(first)->end,
                             sketch.find(second)->start);
    };
    join(r.bottom, r.right);
    join(r.right, r.top);
    join(r.top, r.left);
    join(r.left, r.bottom);
    sketch.addConstraint(ConstraintKind::Horizontal, r.bottom);
    sketch.addConstraint(ConstraintKind::Horizontal, r.top);
    sketch.addConstraint(ConstraintKind::Vertical, r.right);
    sketch.addConstraint(ConstraintKind::Vertical, r.left);
    return r;
}

Point2 startOf(const Sketch& sketch, EntityId line) {
    return sketch.position(sketch.find(line)->start);
}
Point2 endOf(const Sketch& sketch, EntityId line) {
    return sketch.position(sketch.find(line)->end);
}
Point2 centerOf(const Sketch& sketch, EntityId round) {
    return sketch.position(sketch.find(round)->center);
}
double lengthOf(const Sketch& sketch, EntityId line) {
    const Point2 a = startOf(sketch, line);
    const Point2 b = endOf(sketch, line);
    return std::hypot(b.x - a.x, b.y - a.y);
}
double distanceBetween(Point2 a, Point2 b) {
    return std::hypot(b.x - a.x, b.y - a.y);
}
// The sine of the angle between two lines: zero when they are parallel.
double sineBetween(const Sketch& sketch, EntityId first, EntityId second) {
    const Point2 a = startOf(sketch, first), b = endOf(sketch, first);
    const Point2 c = startOf(sketch, second), d = endOf(sketch, second);
    return ((b.x - a.x) * (d.y - c.y) - (b.y - a.y) * (d.x - c.x)) /
           (lengthOf(sketch, first) * lengthOf(sketch, second));
}
double cosineBetween(const Sketch& sketch, EntityId first, EntityId second) {
    const Point2 a = startOf(sketch, first), b = endOf(sketch, first);
    const Point2 c = startOf(sketch, second), d = endOf(sketch, second);
    return ((b.x - a.x) * (d.x - c.x) + (b.y - a.y) * (d.y - c.y)) /
           (lengthOf(sketch, first) * lengthOf(sketch, second));
}
// How far a point is from the infinite line through a line entity.
double distanceToLine(const Sketch& sketch, Point2 p, EntityId line) {
    const Point2 a = startOf(sketch, line), b = endOf(sketch, line);
    return std::abs((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)) / lengthOf(sketch, line);
}

} // namespace

// The acceptance test of Phase 2, at the level of the sketch: a fully constrained rectangle
// whose geometry follows its dimensions.
TEST_CASE("A fully constrained rectangle follows its dimensions", "[sketch][solver]") {
    Sketch sketch;
    Rectangle r = addRectangle(sketch);
    sketch.addConstraint(ConstraintKind::Coincident, sketch.find(r.bottom)->start, sketch.origin());
    r.width = sketch.addConstraint(ConstraintKind::Distance, sketch.find(r.bottom)->start,
                                   sketch.find(r.bottom)->end, 0, 40.0);
    r.height = sketch.addConstraint(ConstraintKind::Distance, sketch.find(r.right)->start,
                                    sketch.find(r.right)->end, 0, 25.0);

    SolveResult result = sketch.solve();

    CHECK(result.status == SolveStatus::Solved);
    CHECK(result.degreesOfFreedom == 0);
    CHECK(result.isFullyConstrained());
    CHECK(result.underconstrained.empty());
    CHECK(result.redundant.empty());
    CHECK(result.conflicting.empty());
    const auto checkCorners = [&](double width, double height) {
        CHECK_THAT(startOf(sketch, r.bottom).x, WithinAbs(0.0, lengthTolerance));
        CHECK_THAT(startOf(sketch, r.bottom).y, WithinAbs(0.0, lengthTolerance));
        CHECK_THAT(endOf(sketch, r.bottom).x, WithinAbs(width, lengthTolerance));
        CHECK_THAT(endOf(sketch, r.bottom).y, WithinAbs(0.0, lengthTolerance));
        CHECK_THAT(endOf(sketch, r.right).x, WithinAbs(width, lengthTolerance));
        CHECK_THAT(endOf(sketch, r.right).y, WithinAbs(height, lengthTolerance));
        CHECK_THAT(endOf(sketch, r.top).x, WithinAbs(0.0, lengthTolerance));
        CHECK_THAT(endOf(sketch, r.top).y, WithinAbs(height, lengthTolerance));
        // The corners are still joined.
        CHECK(distanceBetween(endOf(sketch, r.left), startOf(sketch, r.bottom)) < lengthTolerance);
        const std::vector<Profile> profiles = sketch.profiles();
        REQUIRE(profiles.size() == 1);
        CHECK_THAT(profiles[0].area, WithinAbs(width * height, 1e-5));
    };
    checkCorners(40.0, 25.0);

    // Change the dimensions: the geometry updates, and stays fully constrained.
    sketch.setConstraintValue(r.width, 62.5);
    sketch.setConstraintValue(r.height, 10.0);
    result = sketch.solve();
    CHECK(result.isFullyConstrained());
    checkCorners(62.5, 10.0);

    sketch.setConstraintValue(r.width, 3.0);
    result = sketch.solve();
    CHECK(result.isFullyConstrained());
    checkCorners(3.0, 10.0);
}

TEST_CASE("Degrees of freedom go down as constraints are added", "[sketch][solver]") {
    Sketch sketch;
    CHECK(sketch.state().degreesOfFreedom == 0);

    // A line can move four ways: each end, in X and in Y.
    const EntityId line = sketch.addLine({1.0, 1.0}, {9.0, 3.0});
    CHECK(sketch.state().degreesOfFreedom == 4);
    CHECK(sketch.state().underconstrained == std::vector<EntityId>{line});

    sketch.addConstraint(ConstraintKind::Horizontal, line);
    CHECK(sketch.solve().degreesOfFreedom == 3);

    sketch.addConstraint(ConstraintKind::Distance, sketch.find(line)->start, sketch.find(line)->end,
                         0, 12.0);
    CHECK(sketch.solve().degreesOfFreedom == 2);

    sketch.addConstraint(ConstraintKind::Coincident, sketch.find(line)->start, sketch.origin());
    const SolveResult result = sketch.solve();
    CHECK(result.degreesOfFreedom == 0);
    CHECK(result.isFullyConstrained());
    CHECK(result.underconstrained.empty());
    CHECK_THAT(endOf(sketch, line).x, WithinAbs(12.0, lengthTolerance));
    CHECK_THAT(endOf(sketch, line).y, WithinAbs(0.0, lengthTolerance));

    // A circle adds three: its center, in X and in Y, and its radius. A point adds two.
    const EntityId circle = sketch.addCircle({5.0, 5.0}, 2.0);
    const EntityId point = sketch.addPoint({7.0, 7.0});
    CHECK(sketch.state().degreesOfFreedom == 5);
    CHECK(sketch.state().underconstrained == std::vector<EntityId>{circle, point});
    // An arc adds five: three points, less the rule that its ends are equally far from its center.
    sketch.addArc({20.0, 0.0}, {25.0, 0.0}, {20.0, 5.0});
    CHECK(sketch.state().degreesOfFreedom == 10);
}

TEST_CASE("The solver says which geometry can still move", "[sketch][solver]") {
    Sketch sketch;
    Rectangle r = addRectangle(sketch);
    SolveResult result = sketch.solve();

    // Joined and squared up, a rectangle can still slide two ways and change two sizes.
    CHECK(result.status == SolveStatus::Solved);
    CHECK(result.degreesOfFreedom == 4);
    CHECK(result.underconstrained == (std::vector<EntityId>{r.bottom, r.right, r.top, r.left}));

    // Pin one corner and give the width: only the height is left, and with it the top and the
    // two sides. The bottom cannot move any more.
    sketch.addConstraint(ConstraintKind::Coincident, sketch.find(r.bottom)->start, sketch.origin());
    sketch.addConstraint(ConstraintKind::Distance, sketch.find(r.bottom)->start,
                         sketch.find(r.bottom)->end, 0, 40.0);
    result = sketch.solve();
    CHECK(result.degreesOfFreedom == 1);
    CHECK(result.underconstrained == (std::vector<EntityId>{r.right, r.top, r.left}));
    CHECK_FALSE(result.isFullyConstrained());
}

TEST_CASE("Geometry that does not have to move stays where it is", "[sketch][solver]") {
    Sketch sketch;
    const EntityId line = sketch.addLine({0.0, 0.0}, {10.0, 1.0});
    const EntityId distant = sketch.addLine({100.0, 50.0}, {120.0, 70.0});
    const EntityId circle = sketch.addCircle({-30.0, 4.0}, 6.0);
    sketch.addConstraint(ConstraintKind::Horizontal, line);

    sketch.solve();

    // The crooked line is level now, about where it was.
    CHECK_THAT(startOf(sketch, line).y, WithinAbs(endOf(sketch, line).y, lengthTolerance));
    CHECK_THAT(startOf(sketch, line).y, WithinAbs(0.5, lengthTolerance));
    CHECK_THAT(startOf(sketch, line).x, WithinAbs(0.0, lengthTolerance));
    // And nothing else moved at all.
    CHECK(startOf(sketch, distant).x == 100.0);
    CHECK(endOf(sketch, distant).y == 70.0);
    CHECK(centerOf(sketch, circle).x == -30.0);
    CHECK(sketch.radiusOf(circle) == 6.0);

    // Solving what is already solved changes nothing.
    const Sketch before = sketch;
    sketch.solve();
    CHECK(startOf(sketch, line).y == startOf(before, line).y);
    CHECK(endOf(sketch, line).x == endOf(before, line).x);
}

TEST_CASE("state() reports without moving anything", "[sketch][solver]") {
    Sketch sketch;
    const EntityId line = sketch.addLine({0.0, 0.0}, {10.0, 1.0});
    sketch.addConstraint(ConstraintKind::Horizontal, line);

    const SolveResult state = sketch.state();

    CHECK(state.degreesOfFreedom == 3);
    CHECK(endOf(sketch, line).y == 1.0);
}

TEST_CASE("Each geometric constraint is met after solving", "[sketch][solver][constraints]") {
    Sketch sketch;

    SECTION("coincident points") {
        const EntityId a = sketch.addPoint({1.0, 1.0});
        const EntityId b = sketch.addPoint({4.0, 5.0});
        sketch.addConstraint(ConstraintKind::Coincident, a, b);
        CHECK(sketch.solve().status == SolveStatus::Solved);
        CHECK(distanceBetween(sketch.position(a), sketch.position(b)) < lengthTolerance);
        // They met in the middle: neither had more reason to move than the other.
        CHECK_THAT(sketch.position(a).x, WithinAbs(2.5, lengthTolerance));
    }
    SECTION("a point on a line") {
        const EntityId line = sketch.addLine({0.0, 0.0}, {10.0, 10.0});
        const EntityId p = sketch.addPoint({6.0, 1.0});
        sketch.addConstraint(ConstraintKind::PointOnLine, p, line);
        sketch.solve();
        CHECK(distanceToLine(sketch, sketch.position(p), line) < lengthTolerance);
    }
    SECTION("a point on a circle, and on an arc") {
        const EntityId circle = sketch.addCircle({0.0, 0.0}, 5.0);
        const EntityId arc = sketch.addArc({20.0, 0.0}, {24.0, 0.0}, {20.0, 4.0});
        const EntityId p = sketch.addPoint({9.0, 1.0});
        const EntityId q = sketch.addPoint({26.0, 3.0});
        sketch.addConstraint(ConstraintKind::PointOnCircle, p, circle);
        sketch.addConstraint(ConstraintKind::PointOnCircle, q, arc);
        sketch.solve();
        CHECK_THAT(distanceBetween(sketch.position(p), centerOf(sketch, circle)),
                   WithinAbs(sketch.radiusOf(circle), lengthTolerance));
        CHECK_THAT(distanceBetween(sketch.position(q), centerOf(sketch, arc)),
                   WithinAbs(sketch.radiusOf(arc), lengthTolerance));
    }
    SECTION("horizontal and vertical, for lines and for pairs of points") {
        const EntityId level = sketch.addLine({0.0, 0.0}, {10.0, 2.0});
        const EntityId upright = sketch.addLine({5.0, 0.0}, {6.0, 9.0});
        const EntityId a = sketch.addPoint({20.0, 3.0});
        const EntityId b = sketch.addPoint({30.0, 5.0});
        const EntityId c = sketch.addPoint({31.0, 15.0});
        sketch.addConstraint(ConstraintKind::Horizontal, level);
        sketch.addConstraint(ConstraintKind::Vertical, upright);
        sketch.addConstraint(ConstraintKind::Horizontal, a, b);
        sketch.addConstraint(ConstraintKind::Vertical, b, c);
        sketch.solve();
        CHECK_THAT(startOf(sketch, level).y, WithinAbs(endOf(sketch, level).y, lengthTolerance));
        CHECK_THAT(startOf(sketch, upright).x,
                   WithinAbs(endOf(sketch, upright).x, lengthTolerance));
        CHECK_THAT(sketch.position(a).y, WithinAbs(sketch.position(b).y, lengthTolerance));
        CHECK_THAT(sketch.position(b).x, WithinAbs(sketch.position(c).x, lengthTolerance));
    }
    SECTION("parallel and perpendicular") {
        const EntityId base = sketch.addLine({0.0, 0.0}, {10.0, 3.0});
        const EntityId along = sketch.addLine({0.0, 5.0}, {9.0, 9.0});
        const EntityId across = sketch.addLine({20.0, 0.0}, {21.0, 8.0});
        sketch.addConstraint(ConstraintKind::Parallel, base, along);
        sketch.addConstraint(ConstraintKind::Perpendicular, base, across);
        sketch.solve();
        CHECK_THAT(sineBetween(sketch, base, along), WithinAbs(0.0, angleTolerance));
        CHECK_THAT(cosineBetween(sketch, base, across), WithinAbs(0.0, angleTolerance));
        // Still pointing the way they did, not flipped.
        CHECK(cosineBetween(sketch, base, along) > 0.99);
    }
    SECTION("a line tangent to a circle, on the side it was drawn") {
        const EntityId circle = sketch.addCircle({0.0, 0.0}, 5.0);
        const EntityId above = sketch.addLine({-10.0, 6.5}, {10.0, 7.0});
        const EntityId below = sketch.addLine({-10.0, -4.0}, {10.0, -3.5});
        sketch.addConstraint(ConstraintKind::Tangent, above, circle);
        sketch.addConstraint(ConstraintKind::Tangent, circle, below);
        sketch.solve();
        const double radius = sketch.radiusOf(circle);
        CHECK_THAT(distanceToLine(sketch, centerOf(sketch, circle), above),
                   WithinAbs(radius, lengthTolerance));
        CHECK_THAT(distanceToLine(sketch, centerOf(sketch, circle), below),
                   WithinAbs(radius, lengthTolerance));
        CHECK(startOf(sketch, above).y > centerOf(sketch, circle).y);
        CHECK(startOf(sketch, below).y < centerOf(sketch, circle).y);
    }
    SECTION("circles tangent from outside and from inside") {
        const EntityId a = sketch.addCircle({0.0, 0.0}, 5.0);
        const EntityId outside = sketch.addCircle({9.0, 0.0}, 3.0);
        const EntityId inside = sketch.addCircle({1.0, 0.5}, 2.0);
        sketch.addConstraint(ConstraintKind::Tangent, a, outside);
        sketch.addConstraint(ConstraintKind::Tangent, a, inside);
        sketch.solve();
        CHECK_THAT(distanceBetween(centerOf(sketch, a), centerOf(sketch, outside)),
                   WithinAbs(sketch.radiusOf(a) + sketch.radiusOf(outside), lengthTolerance));
        CHECK_THAT(distanceBetween(centerOf(sketch, a), centerOf(sketch, inside)),
                   WithinAbs(sketch.radiusOf(a) - sketch.radiusOf(inside), lengthTolerance));
    }
    SECTION("a line tangent to an arc") {
        const EntityId arc = sketch.addArc({0.0, 0.0}, {5.0, 0.0}, {0.0, 5.0});
        const EntityId line = sketch.addLine({5.5, -3.0}, {5.0, 6.0});
        sketch.addConstraint(ConstraintKind::Tangent, line, arc);
        sketch.solve();
        CHECK_THAT(distanceToLine(sketch, centerOf(sketch, arc), line),
                   WithinAbs(sketch.radiusOf(arc), lengthTolerance));
    }
    SECTION("equal lengths and equal radii") {
        const EntityId a = sketch.addLine({0.0, 0.0}, {10.0, 0.0});
        const EntityId b = sketch.addLine({0.0, 5.0}, {3.0, 9.0});
        const EntityId c = sketch.addCircle({20.0, 0.0}, 4.0);
        const EntityId d = sketch.addArc({40.0, 0.0}, {46.0, 0.0}, {40.0, 6.0});
        sketch.addConstraint(ConstraintKind::Equal, a, b);
        sketch.addConstraint(ConstraintKind::Equal, c, d);
        sketch.solve();
        CHECK_THAT(lengthOf(sketch, a), WithinAbs(lengthOf(sketch, b), lengthTolerance));
        CHECK_THAT(sketch.radiusOf(c), WithinAbs(sketch.radiusOf(d), lengthTolerance));
    }
    SECTION("concentric") {
        const EntityId a = sketch.addCircle({0.0, 0.0}, 4.0);
        const EntityId b = sketch.addArc({3.0, 1.0}, {9.0, 1.0}, {3.0, 7.0});
        sketch.addConstraint(ConstraintKind::Concentric, a, b);
        sketch.solve();
        CHECK(distanceBetween(centerOf(sketch, a), centerOf(sketch, b)) < lengthTolerance);
        // The circle's radius is its own and is left alone. The arc's is the distance from its
        // center to its ends, which have not moved, so it has changed; its ends are still
        // equally far from the center.
        CHECK_THAT(sketch.radiusOf(a), WithinAbs(4.0, lengthTolerance));
        CHECK_THAT(distanceBetween(centerOf(sketch, b), sketch.position(sketch.find(b)->end)),
                   WithinAbs(sketch.radiusOf(b), lengthTolerance));
    }
    SECTION("midpoint") {
        const EntityId line = sketch.addLine({0.0, 0.0}, {10.0, 4.0});
        const EntityId p = sketch.addPoint({6.0, 6.0});
        sketch.addConstraint(ConstraintKind::Midpoint, p, line);
        sketch.solve();
        CHECK_THAT(
            sketch.position(p).x,
            WithinAbs((startOf(sketch, line).x + endOf(sketch, line).x) / 2.0, lengthTolerance));
        CHECK_THAT(
            sketch.position(p).y,
            WithinAbs((startOf(sketch, line).y + endOf(sketch, line).y) / 2.0, lengthTolerance));
    }
    SECTION("symmetric about a line") {
        const EntityId axis = sketch.addLine({0.0, -10.0}, {0.0, 10.0});
        sketch.addConstraint(ConstraintKind::Fix, sketch.find(axis)->start);
        sketch.addConstraint(ConstraintKind::Fix, sketch.find(axis)->end);
        const EntityId p = sketch.addPoint({-4.0, 2.0});
        const EntityId q = sketch.addPoint({5.0, 3.0});
        sketch.addConstraint(ConstraintKind::Symmetric, p, q, axis);
        sketch.solve();
        CHECK_THAT(sketch.position(p).x, WithinAbs(-sketch.position(q).x, lengthTolerance));
        CHECK_THAT(sketch.position(p).y, WithinAbs(sketch.position(q).y, lengthTolerance));
        CHECK(sketch.position(q).x > 1.0);
    }
    SECTION("collinear") {
        const EntityId a = sketch.addLine({0.0, 0.0}, {10.0, 1.0});
        const EntityId b = sketch.addLine({15.0, 3.0}, {25.0, 3.5});
        sketch.addConstraint(ConstraintKind::Collinear, a, b);
        sketch.solve();
        CHECK_THAT(sineBetween(sketch, a, b), WithinAbs(0.0, angleTolerance));
        CHECK(distanceToLine(sketch, startOf(sketch, b), a) < lengthTolerance);
        CHECK(distanceToLine(sketch, endOf(sketch, b), a) < lengthTolerance);
    }
    SECTION("fix holds a point against everything else") {
        const EntityId line = sketch.addLine({2.0, 3.0}, {10.0, 4.0});
        sketch.addConstraint(ConstraintKind::Fix, sketch.find(line)->start);
        sketch.addConstraint(ConstraintKind::Horizontal, line);
        sketch.solve();
        CHECK(startOf(sketch, line).x == 2.0);
        CHECK(startOf(sketch, line).y == 3.0);
        CHECK_THAT(endOf(sketch, line).y, WithinAbs(3.0, lengthTolerance));
    }
}

TEST_CASE("Each dimension is met after solving", "[sketch][solver][constraints]") {
    Sketch sketch;

    SECTION("distance between points") {
        const EntityId line = sketch.addLine({0.0, 0.0}, {3.0, 4.0});
        sketch.addConstraint(ConstraintKind::Distance, sketch.find(line)->start,
                             sketch.find(line)->end, 0, 20.0);
        sketch.solve();
        CHECK_THAT(lengthOf(sketch, line), WithinAbs(20.0, lengthTolerance));
        // It grew along its own direction, about its middle.
        CHECK_THAT(sineBetween(sketch, line, line), WithinAbs(0.0, angleTolerance));
        CHECK_THAT(endOf(sketch, line).y / endOf(sketch, line).x, WithinAbs(4.0 / 3.0, 1e-6));
    }
    SECTION("distance along X and along Y, keeping which side is which") {
        const EntityId a = sketch.addPoint({0.0, 0.0});
        const EntityId right = sketch.addPoint({3.0, 5.0});
        const EntityId left = sketch.addPoint({-2.0, -1.0});
        sketch.addConstraint(ConstraintKind::Fix, a);
        sketch.addConstraint(ConstraintKind::DistanceX, a, right, 0, 12.0);
        sketch.addConstraint(ConstraintKind::DistanceY, a, right, 0, 7.0);
        sketch.addConstraint(ConstraintKind::DistanceX, a, left, 0, 9.0);
        sketch.addConstraint(ConstraintKind::DistanceY, a, left, 0, 4.0);
        CHECK(sketch.solve().isFullyConstrained());
        CHECK_THAT(sketch.position(right).x, WithinAbs(12.0, lengthTolerance));
        CHECK_THAT(sketch.position(right).y, WithinAbs(7.0, lengthTolerance));
        CHECK_THAT(sketch.position(left).x, WithinAbs(-9.0, lengthTolerance));
        CHECK_THAT(sketch.position(left).y, WithinAbs(-4.0, lengthTolerance));
    }
    SECTION("distance from a point to a line, on the side it is on") {
        const EntityId line = sketch.addLine({0.0, 0.0}, {10.0, 0.0});
        sketch.addConstraint(ConstraintKind::Fix, sketch.find(line)->start);
        sketch.addConstraint(ConstraintKind::Fix, sketch.find(line)->end);
        const EntityId p = sketch.addPoint({4.0, -2.0});
        sketch.addConstraint(ConstraintKind::PointLineDistance, p, line, 0, 6.0);
        sketch.solve();
        CHECK_THAT(sketch.position(p).y, WithinAbs(-6.0, lengthTolerance));
        CHECK_THAT(sketch.position(p).x, WithinAbs(4.0, lengthTolerance));
    }
    SECTION("radius and diameter, of a circle and of an arc") {
        const EntityId circle = sketch.addCircle({0.0, 0.0}, 5.0);
        const EntityId arc = sketch.addArc({20.0, 0.0}, {24.0, 0.0}, {20.0, 4.0});
        sketch.addConstraint(ConstraintKind::Diameter, circle, 0, 0, 30.0);
        sketch.addConstraint(ConstraintKind::Radius, arc, 0, 0, 2.5);
        sketch.solve();
        CHECK_THAT(sketch.radiusOf(circle), WithinAbs(15.0, lengthTolerance));
        CHECK_THAT(sketch.radiusOf(arc), WithinAbs(2.5, lengthTolerance));
        // Both ends of the arc came in with it.
        CHECK_THAT(distanceBetween(sketch.position(sketch.find(arc)->end), centerOf(sketch, arc)),
                   WithinAbs(2.5, lengthTolerance));
    }
    SECTION("angle between lines") {
        const EntityId base = sketch.addLine({0.0, 0.0}, {10.0, 0.0});
        sketch.addConstraint(ConstraintKind::Fix, sketch.find(base)->start);
        sketch.addConstraint(ConstraintKind::Fix, sketch.find(base)->end);
        const EntityId arm = sketch.addLine({0.0, 0.0}, {8.0, 5.0});
        const ConstraintId angle =
            sketch.addConstraint(ConstraintKind::Angle, base, arm, 0, 45.0 * pi / 180.0);
        sketch.solve();
        CHECK_THAT(sketch.measure(ConstraintKind::Angle, base, arm),
                   WithinAbs(pi / 4.0, angleTolerance));

        // A large change goes to the angle asked for, not to the one half a turn from it.
        sketch.setConstraintValue(angle, 150.0 * pi / 180.0);
        CHECK(sketch.solve().status == SolveStatus::Solved);
        CHECK_THAT(sketch.measure(ConstraintKind::Angle, base, arm),
                   WithinAbs(150.0 * pi / 180.0, angleTolerance));
    }
}

TEST_CASE("An arc's ends stay on its circle whatever else moves", "[sketch][solver]") {
    Sketch sketch;
    const EntityId arc = sketch.addArc({0.0, 0.0}, {5.0, 0.0}, {0.0, 5.0});
    const EntityId target = sketch.addPoint({9.0, 9.0});
    sketch.addConstraint(ConstraintKind::Fix, target);
    sketch.addConstraint(ConstraintKind::Coincident, sketch.find(arc)->end, target);

    CHECK(sketch.solve().status == SolveStatus::Solved);

    const Point2 center = centerOf(sketch, arc);
    CHECK_THAT(sketch.position(sketch.find(arc)->end).x, WithinAbs(9.0, lengthTolerance));
    CHECK_THAT(distanceBetween(center, sketch.position(sketch.find(arc)->start)),
               WithinAbs(distanceBetween(center, sketch.position(sketch.find(arc)->end)),
                         lengthTolerance));
}

TEST_CASE("A constraint that adds nothing is reported as redundant", "[sketch][solver]") {
    Sketch sketch;
    const EntityId line = sketch.addLine({0.0, 0.0}, {10.0, 1.0});
    const EntityId other = sketch.addLine({0.0, 5.0}, {10.0, 6.0});
    const ConstraintId level = sketch.addConstraint(ConstraintKind::Horizontal, line);
    sketch.addConstraint(ConstraintKind::Horizontal, other);
    CHECK(sketch.solve().status == SolveStatus::Solved);

    SECTION("the same thing said twice") {
        const ConstraintId again = sketch.addConstraint(
            ConstraintKind::Horizontal, sketch.find(line)->start, sketch.find(line)->end);
        const SolveResult result = sketch.solve();
        CHECK(result.status == SolveStatus::Redundant);
        CHECK(result.redundant == std::vector<ConstraintId>{again});
        CHECK(result.conflicting.empty());
        // The one that came first is not the one blamed.
        CHECK(std::find(result.redundant.begin(), result.redundant.end(), level) ==
              result.redundant.end());
        CHECK_FALSE(result.isFullyConstrained());
    }
    SECTION("something that follows from the others") {
        // Two horizontal lines are parallel already.
        const ConstraintId parallel = sketch.addConstraint(ConstraintKind::Parallel, line, other);
        const SolveResult result = sketch.solve();
        CHECK(result.status == SolveStatus::Redundant);
        CHECK(result.redundant == std::vector<ConstraintId>{parallel});
        // Redundant constraints do not change the count of what can still move.
        CHECK(result.degreesOfFreedom == 6);
    }
}

TEST_CASE("Constraints that contradict each other are reported, and the rest still hold",
          "[sketch][solver]") {
    Sketch sketch;
    const EntityId line = sketch.addLine({0.0, 0.0}, {10.0, 0.5});
    const Entity* l = sketch.find(line);
    sketch.addConstraint(ConstraintKind::Coincident, l->start, sketch.origin());
    const ConstraintId level = sketch.addConstraint(ConstraintKind::Horizontal, line);
    const ConstraintId forty =
        sketch.addConstraint(ConstraintKind::Distance, l->start, l->end, 0, 40.0);

    SECTION("two lengths for one line") {
        const ConstraintId fifty =
            sketch.addConstraint(ConstraintKind::Distance, l->start, l->end, 0, 50.0);
        const SolveResult result = sketch.solve();
        CHECK(result.status == SolveStatus::Conflicting);
        // The later one is the one that could not be added to what was there.
        CHECK(result.conflicting == std::vector<ConstraintId>{fifty});
        CHECK_THAT(lengthOf(sketch, line), WithinAbs(40.0, lengthTolerance));
        CHECK_THAT(endOf(sketch, line).y, WithinAbs(0.0, lengthTolerance));
        CHECK_FALSE(result.isFullyConstrained());
        // Taking it out again leaves a sound sketch.
        sketch.removeConstraint(fifty);
        CHECK(sketch.solve().isFullyConstrained());
    }
    SECTION("level and upright at once, for a line with a length") {
        const ConstraintId upright = sketch.addConstraint(ConstraintKind::Vertical, line);
        const SolveResult result = sketch.solve();
        CHECK(result.status == SolveStatus::Conflicting);
        CHECK(result.conflicting == std::vector<ConstraintId>{upright});
        CHECK_THAT(lengthOf(sketch, line), WithinAbs(40.0, lengthTolerance));
        CHECK(std::find(result.conflicting.begin(), result.conflicting.end(), level) ==
              result.conflicting.end());
        (void)forty;
    }
    SECTION("a triangle whose sides cannot meet") {
        Sketch triangle;
        const EntityId a = triangle.addPoint({0.0, 0.0});
        const EntityId b = triangle.addPoint({10.0, 0.0});
        const EntityId c = triangle.addPoint({5.0, 5.0});
        triangle.addConstraint(ConstraintKind::Distance, a, b, 0, 10.0);
        triangle.addConstraint(ConstraintKind::Distance, b, c, 0, 3.0);
        const ConstraintId tooShort =
            triangle.addConstraint(ConstraintKind::Distance, a, c, 0, 4.0);
        const SolveResult result = triangle.solve();
        CHECK(result.status == SolveStatus::Conflicting);
        CHECK(result.conflicting == std::vector<ConstraintId>{tooShort});
        CHECK_THAT(distanceBetween(triangle.position(a), triangle.position(b)),
                   WithinAbs(10.0, lengthTolerance));
        CHECK_THAT(distanceBetween(triangle.position(b), triangle.position(c)),
                   WithinAbs(3.0, lengthTolerance));
    }
}

TEST_CASE("Dragging moves what is free and nothing else", "[sketch][solver][drag]") {
    Sketch sketch;

    SECTION("a free point goes where it is dragged") {
        const EntityId p = sketch.addPoint({1.0, 1.0});
        sketch.dragBy(p, 4.0, -2.5);
        CHECK_THAT(sketch.position(p).x, WithinAbs(5.0, lengthTolerance));
        CHECK_THAT(sketch.position(p).y, WithinAbs(-1.5, lengthTolerance));
    }
    SECTION("the end of a level line pinned at its start slides along, and stays level") {
        const EntityId line = sketch.addLine({0.0, 0.0}, {10.0, 0.0});
        sketch.addConstraint(ConstraintKind::Coincident, sketch.find(line)->start, sketch.origin());
        sketch.addConstraint(ConstraintKind::Horizontal, line);
        const SolveResult result = sketch.dragBy(sketch.find(line)->end, 7.0, 5.0);
        CHECK(result.status == SolveStatus::Solved);
        CHECK_THAT(endOf(sketch, line).x, WithinAbs(17.0, lengthTolerance));
        CHECK_THAT(endOf(sketch, line).y, WithinAbs(0.0, lengthTolerance));
        CHECK_THAT(startOf(sketch, line).x, WithinAbs(0.0, lengthTolerance));
    }
    SECTION("the end of a line of fixed length swings around its pinned start") {
        const EntityId line = sketch.addLine({0.0, 0.0}, {10.0, 0.0});
        sketch.addConstraint(ConstraintKind::Coincident, sketch.find(line)->start, sketch.origin());
        sketch.addConstraint(ConstraintKind::Distance, sketch.find(line)->start,
                             sketch.find(line)->end, 0, 10.0);
        sketch.dragBy(sketch.find(line)->end, 0.0, 10.0);
        CHECK_THAT(lengthOf(sketch, line), WithinAbs(10.0, lengthTolerance));
        // Toward where it was dragged: up and to the right of the start, at 45 degrees.
        CHECK_THAT(endOf(sketch, line).x, WithinAbs(10.0 / std::sqrt(2.0), 1e-3));
        CHECK_THAT(endOf(sketch, line).y, WithinAbs(10.0 / std::sqrt(2.0), 1e-3));
    }
    SECTION("a line dragged as a whole keeps its length and direction") {
        const EntityId line = sketch.addLine({0.0, 0.0}, {10.0, 4.0});
        sketch.dragBy(line, 3.0, 3.0);
        CHECK_THAT(startOf(sketch, line).x, WithinAbs(3.0, lengthTolerance));
        CHECK_THAT(startOf(sketch, line).y, WithinAbs(3.0, lengthTolerance));
        CHECK_THAT(endOf(sketch, line).x, WithinAbs(13.0, lengthTolerance));
        CHECK_THAT(endOf(sketch, line).y, WithinAbs(7.0, lengthTolerance));
    }
    SECTION("fully constrained geometry does not give") {
        Rectangle r = addRectangle(sketch);
        sketch.addConstraint(ConstraintKind::Coincident, sketch.find(r.bottom)->start,
                             sketch.origin());
        sketch.addConstraint(ConstraintKind::Distance, sketch.find(r.bottom)->start,
                             sketch.find(r.bottom)->end, 0, 40.0);
        sketch.addConstraint(ConstraintKind::Distance, sketch.find(r.right)->start,
                             sketch.find(r.right)->end, 0, 25.0);
        REQUIRE(sketch.solve().isFullyConstrained());
        const SolveResult result = sketch.dragBy(sketch.find(r.top)->start, 12.0, 9.0);
        CHECK(result.isFullyConstrained());
        CHECK_THAT(startOf(sketch, r.top).x, WithinAbs(40.0, lengthTolerance));
        CHECK_THAT(startOf(sketch, r.top).y, WithinAbs(25.0, lengthTolerance));
    }
    SECTION("dragging one corner of a loose rectangle keeps it a rectangle") {
        Rectangle r = addRectangle(sketch);
        sketch.addConstraint(ConstraintKind::Coincident, sketch.find(r.bottom)->start,
                             sketch.origin());
        sketch.solve();
        sketch.dragBy(sketch.find(r.right)->end, 10.0, 5.0);
        // Still joined, level, and upright; the pinned corner has not moved.
        CHECK(distanceBetween(endOf(sketch, r.right), startOf(sketch, r.top)) < lengthTolerance);
        CHECK_THAT(startOf(sketch, r.top).y, WithinAbs(endOf(sketch, r.top).y, lengthTolerance));
        CHECK_THAT(startOf(sketch, r.right).x,
                   WithinAbs(endOf(sketch, r.right).x, lengthTolerance));
        CHECK_THAT(startOf(sketch, r.bottom).x, WithinAbs(0.0, lengthTolerance));
        // And the corner followed the pointer: the rectangle is wider and taller by that much.
        CHECK_THAT(endOf(sketch, r.right).x, WithinAbs(38.75 + 10.0, 0.6));
        CHECK(sketch.profiles().size() == 1);
    }
    SECTION("a circle's radius follows the pointer unless a dimension holds it") {
        const EntityId circle = sketch.addCircle({0.0, 0.0}, 5.0);
        sketch.dragRadius(circle, {0.0, 12.0});
        CHECK_THAT(sketch.radiusOf(circle), WithinAbs(12.0, lengthTolerance));
        CHECK_THAT(centerOf(sketch, circle).x, WithinAbs(0.0, lengthTolerance));

        sketch.addConstraint(ConstraintKind::Radius, circle, 0, 0, 12.0);
        sketch.dragRadius(circle, {0.0, 30.0});
        CHECK_THAT(sketch.radiusOf(circle), WithinAbs(12.0, lengthTolerance));
    }
    SECTION("the origin cannot be dragged") {
        sketch.dragBy(sketch.origin(), 5.0, 5.0);
        CHECK(sketch.position(sketch.origin()).x == 0.0);
        CHECK_THROWS_AS(sketch.dragBy(999, 1.0, 1.0), std::invalid_argument);
    }
}

TEST_CASE("Sketches far from the origin and of very different sizes still solve",
          "[sketch][solver]") {
    const double scale = GENERATE(0.01, 1.0, 250.0);
    const double offset = GENERATE(0.0, 5000.0);
    CAPTURE(scale, offset);
    Sketch sketch;
    const EntityId bottom =
        sketch.addLine({offset, offset}, {offset + 4.1 * scale, offset + 0.2 * scale});
    const EntityId side = sketch.addLine({offset + 4.0 * scale, offset},
                                         {offset + 3.9 * scale, offset + 3.0 * scale});
    const EntityId circle = sketch.addCircle({offset + 2.0 * scale, offset + 1.4 * scale}, scale);
    sketch.addConstraint(ConstraintKind::Fix, sketch.find(bottom)->start);
    sketch.addConstraint(ConstraintKind::Coincident, sketch.find(bottom)->end,
                         sketch.find(side)->start);
    sketch.addConstraint(ConstraintKind::Horizontal, bottom);
    sketch.addConstraint(ConstraintKind::Perpendicular, bottom, side);
    sketch.addConstraint(ConstraintKind::Distance, sketch.find(bottom)->start,
                         sketch.find(bottom)->end, 0, 4.0 * scale);
    sketch.addConstraint(ConstraintKind::Distance, sketch.find(side)->start, sketch.find(side)->end,
                         0, 3.0 * scale);
    sketch.addConstraint(ConstraintKind::Tangent, bottom, circle);
    sketch.addConstraint(ConstraintKind::Tangent, side, circle);
    sketch.addConstraint(ConstraintKind::Radius, circle, 0, 0, scale);

    const SolveResult result = sketch.solve();

    CHECK(result.isFullyConstrained());
    CHECK_THAT(endOf(sketch, side).x, WithinAbs(offset + 4.0 * scale, 1e-6 * std::max(1.0, scale)));
    CHECK_THAT(endOf(sketch, side).y, WithinAbs(offset + 3.0 * scale, 1e-6 * std::max(1.0, scale)));
    // The circle sits in the corner, touching both lines.
    CHECK_THAT(centerOf(sketch, circle).x,
               WithinAbs(offset + 3.0 * scale, 1e-6 * std::max(1.0, scale)));
    CHECK_THAT(centerOf(sketch, circle).y,
               WithinAbs(offset + 1.0 * scale, 1e-6 * std::max(1.0, scale)));
}

// A line that ends on a circle and is tangent to it there is the commonest thing in a sketch
// after a rectangle: every rounded corner and every slot has it. "As far from the center as the
// radius" cannot tell such a line from one turned slightly about its end, so these check the
// count of freedoms, not just that the constraint is met.
TEST_CASE("Tangency at a shared end takes away exactly one freedom", "[sketch][solver]") {
    Sketch sketch;

    SECTION("a line running into an arc") {
        // A quarter arc around (10, 5) from (10, 0) up to (15, 5), and a line arriving at its
        // start a little off tangent.
        const EntityId arc = sketch.addArc({10.0, 5.0}, {10.0, 0.0}, {15.0, 5.0});
        const EntityId line = sketch.addLine({0.0, 0.6}, {10.0, 0.0});
        sketch.addConstraint(ConstraintKind::Coincident, sketch.find(line)->end,
                             sketch.find(arc)->start);
        const int before = sketch.solve().degreesOfFreedom;
        sketch.addConstraint(ConstraintKind::Tangent, line, arc);
        const SolveResult result = sketch.solve();

        CHECK(result.status == SolveStatus::Solved);
        CHECK(result.degreesOfFreedom == before - 1);
        // Tangent: the line is square to the radius at the shared end.
        const Point2 a = startOf(sketch, line);
        const Point2 b = endOf(sketch, line);
        const Point2 c = centerOf(sketch, arc);
        const double along = ((b.x - a.x) * (b.x - c.x) + (b.y - a.y) * (b.y - c.y)) /
                             (lengthOf(sketch, line) * sketch.radiusOf(arc));
        CHECK_THAT(along, WithinAbs(0.0, angleTolerance));
        CHECK_THAT(distanceBetween(b, sketch.position(sketch.find(arc)->start)),
                   WithinAbs(0.0, lengthTolerance));
    }

    SECTION("a slot: two lines and two arcs, fully constrained by three dimensions") {
        // Centers at (0, 0) and (30, 0), radius 5.
        const EntityId left = sketch.addArc({0.0, 0.0}, {0.0, 5.0}, {0.0, -5.0});
        const EntityId right = sketch.addArc({30.0, 0.0}, {30.0, -5.0}, {30.0, 5.0});
        const EntityId bottom = sketch.addLine({0.0, -5.0}, {30.0, -5.0});
        const EntityId top = sketch.addLine({30.0, 5.0}, {0.0, 5.0});
        const auto join = [&sketch](EntityId a, EntityId b) {
            sketch.addConstraint(ConstraintKind::Coincident, a, b);
        };
        join(sketch.find(left)->end, sketch.find(bottom)->start);
        join(sketch.find(bottom)->end, sketch.find(right)->start);
        join(sketch.find(right)->end, sketch.find(top)->start);
        join(sketch.find(top)->end, sketch.find(left)->start);
        for (const EntityId line : {bottom, top}) {
            sketch.addConstraint(ConstraintKind::Tangent, line, left);
            sketch.addConstraint(ConstraintKind::Tangent, right, line);
        }
        sketch.addConstraint(ConstraintKind::Equal, left, right);
        SolveResult result = sketch.solve();
        CHECK(result.status == SolveStatus::Solved);
        // Two centers and one radius.
        CHECK(result.degreesOfFreedom == 5);

        sketch.addConstraint(ConstraintKind::Coincident, sketch.find(left)->center,
                             sketch.origin());
        sketch.addConstraint(ConstraintKind::Horizontal, sketch.find(left)->center,
                             sketch.find(right)->center);
        const ConstraintId length =
            sketch.addConstraint(ConstraintKind::Distance, sketch.find(left)->center,
                                 sketch.find(right)->center, 0, 30.0);
        const ConstraintId radius = sketch.addConstraint(ConstraintKind::Radius, left, 0, 0, 5.0);
        result = sketch.solve();
        CHECK(result.isFullyConstrained());
        CHECK(result.underconstrained.empty());

        // And it follows its dimensions.
        sketch.setConstraintValue(length, 50.0);
        sketch.setConstraintValue(radius, 8.0);
        CHECK(sketch.solve().isFullyConstrained());
        CHECK_THAT(centerOf(sketch, right).x, WithinAbs(50.0, lengthTolerance));
        CHECK_THAT(sketch.radiusOf(right), WithinAbs(8.0, lengthTolerance));
        CHECK_THAT(startOf(sketch, top).x, WithinAbs(50.0, lengthTolerance));
        CHECK_THAT(startOf(sketch, top).y, WithinAbs(8.0, lengthTolerance));
        CHECK_THAT(endOf(sketch, bottom).y, WithinAbs(-8.0, lengthTolerance));
        REQUIRE(sketch.profiles().size() == 1);
        CHECK_THAT(sketch.profiles()[0].area, WithinAbs(50.0 * 16.0 + pi * 64.0, 1e-6));
    }

    SECTION("a line whose end lies on a circle") {
        const EntityId circle = sketch.addCircle({0.0, 0.0}, 5.0);
        const EntityId line = sketch.addLine({-12.0, 5.5}, {0.5, 5.0});
        sketch.addConstraint(ConstraintKind::PointOnCircle, sketch.find(line)->end, circle);
        const int before = sketch.solve().degreesOfFreedom;
        sketch.addConstraint(ConstraintKind::Tangent, circle, line);
        const SolveResult result = sketch.solve();
        CHECK(result.status == SolveStatus::Solved);
        CHECK(result.degreesOfFreedom == before - 1);
        CHECK_THAT(distanceToLine(sketch, centerOf(sketch, circle), line),
                   WithinAbs(sketch.radiusOf(circle), lengthTolerance));
        CHECK_THAT(distanceBetween(endOf(sketch, line), centerOf(sketch, circle)),
                   WithinAbs(sketch.radiusOf(circle), lengthTolerance));
        // Exactly tangent, which means square to the radius at the end. A line a hundredth of
        // a degree off would pass the distance checks above.
        const Point2 a = startOf(sketch, line);
        const Point2 b = endOf(sketch, line);
        const Point2 c = centerOf(sketch, circle);
        const double along = ((b.x - a.x) * (b.x - c.x) + (b.y - a.y) * (b.y - c.y)) /
                             (lengthOf(sketch, line) * sketch.radiusOf(circle));
        CHECK_THAT(along, WithinAbs(0.0, angleTolerance));
    }

    SECTION("two arcs meeting end to end") {
        // An S: a half turn around (0, 0), then one around (10, 0) the other way.
        const EntityId first = sketch.addArc({0.0, 0.0}, {5.0, 0.0}, {-5.0, 0.0});
        const EntityId second = sketch.addArc({10.2, 0.8}, {5.0, 0.0}, {15.0, 0.5});
        sketch.addConstraint(ConstraintKind::Coincident, sketch.find(first)->start,
                             sketch.find(second)->start);
        const int before = sketch.solve().degreesOfFreedom;
        sketch.addConstraint(ConstraintKind::Tangent, first, second);
        const SolveResult result = sketch.solve();
        CHECK(result.status == SolveStatus::Solved);
        CHECK(result.degreesOfFreedom == before - 1);
        // The centers and the shared end are in line, with the end between the centers.
        const Point2 c1 = centerOf(sketch, first);
        const Point2 c2 = centerOf(sketch, second);
        CHECK_THAT(distanceBetween(c1, c2),
                   WithinAbs(sketch.radiusOf(first) + sketch.radiusOf(second), lengthTolerance));
        const Point2 meet = sketch.position(sketch.find(first)->start);
        const double turned = ((meet.x - c1.x) * (c2.y - c1.y) - (meet.y - c1.y) * (c2.x - c1.x)) /
                              (sketch.radiusOf(first) * distanceBetween(c1, c2));
        CHECK_THAT(turned, WithinAbs(0.0, angleTolerance));
    }
}
