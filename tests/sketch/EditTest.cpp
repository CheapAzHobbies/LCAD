// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/sketch/Edit.h>

#include <Tolerances.h>

#include <catch2/catch_test_macros.hpp>
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

Point2 startOf(const Sketch& sketch, EntityId curve) {
    return sketch.position(sketch.find(curve)->start);
}
Point2 endOf(const Sketch& sketch, EntityId curve) {
    return sketch.position(sketch.find(curve)->end);
}
Point2 centerOf(const Sketch& sketch, EntityId round) {
    return sketch.position(sketch.find(round)->center);
}
double apart(Point2 a, Point2 b) {
    return std::hypot(b.x - a.x, b.y - a.y);
}
double lengthOf(const Sketch& sketch, EntityId line) {
    return apart(startOf(sketch, line), endOf(sketch, line));
}
int curveCount(const Sketch& sketch) {
    return static_cast<int>(
        std::count_if(sketch.entities().begin(), sketch.entities().end(),
                      [](const Entity& entity) { return entity.kind != EntityKind::Point; }));
}
bool hasConstraint(const Sketch& sketch, ConstraintKind kind, EntityId first, EntityId second) {
    return std::any_of(sketch.constraints().begin(), sketch.constraints().end(),
                       [&](const Constraint& c) {
                           return c.kind == kind && c.first == first && c.second == second;
                       });
}
// Whether two sketches hold the same geometry and constraints.
bool same(const Sketch& a, const Sketch& b) {
    if (a.entities().size() != b.entities().size() ||
        a.constraints().size() != b.constraints().size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.entities().size(); ++i) {
        const Entity& x = a.entities()[i];
        const Entity& y = b.entities()[i];
        if (x.id != y.id || x.x != y.x || x.y != y.y || x.radius != y.radius) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("Geometry is found by position, points before curves", "[sketch][edit]") {
    Sketch sketch;
    const EntityId line = sketch.addLine({0.0, 10.0}, {20.0, 10.0});
    const EntityId circle = sketch.addCircle({50.0, 0.0}, 10.0);
    // A quarter turn from (0, -30) to (30, 0) around (0, 0)... the long way is not part of it.
    const EntityId arc = sketch.addArc({0.0, -60.0}, {30.0, -60.0}, {0.0, -30.0});

    CHECK_THAT(distanceTo(sketch, line, {10.0, 13.0}), WithinAbs(3.0, lengthTolerance));
    // Beyond the end of a line, the distance is to the end.
    CHECK_THAT(distanceTo(sketch, line, {24.0, 13.0}), WithinAbs(5.0, lengthTolerance));
    CHECK_THAT(distanceTo(sketch, circle, {50.0, 0.0}), WithinAbs(10.0, lengthTolerance));
    CHECK_THAT(distanceTo(sketch, circle, {50.0, 12.0}), WithinAbs(2.0, lengthTolerance));
    // On the arc's circle, but past its end: the distance is to the end, not to the circle.
    CHECK_THAT(distanceTo(sketch, arc, {-30.0, -60.0}),
               WithinAbs(std::hypot(30.0, 30.0), lengthTolerance));
    CHECK_THAT(distanceTo(sketch, arc, {std::sqrt(450.0) + 1.0, -60.0 + std::sqrt(450.0) + 1.0}),
               WithinAbs(std::sqrt(2.0), lengthTolerance));
    CHECK_THAT(distanceTo(sketch, sketch.origin(), {3.0, 4.0}), WithinAbs(5.0, lengthTolerance));

    const Point2 on = nearestOn(sketch, line, {7.0, 99.0});
    CHECK_THAT(on.x, WithinAbs(7.0, lengthTolerance));
    CHECK_THAT(on.y, WithinAbs(10.0, lengthTolerance));
    CHECK_THROWS_AS(nearestOn(sketch, sketch.origin(), {0.0, 0.0}), std::invalid_argument);

    // Near the line's end both the end point and the line are in reach; the point comes first.
    const std::vector<EntityId> found = entitiesAt(sketch, {19.8, 10.1}, 0.5);
    REQUIRE(found.size() == 2);
    CHECK(found[0] == sketch.find(line)->end);
    CHECK(found[1] == line);
    CHECK(entitiesAt(sketch, {10.0, 10.2}, 0.5) == std::vector<EntityId>{line});
    CHECK(entitiesAt(sketch, {10.0, 11.0}, 0.5).empty());
}

TEST_CASE("Curves cross where both actually are", "[sketch][edit]") {
    Sketch sketch;
    const EntityId flat = sketch.addLine({0.0, 0.0}, {20.0, 0.0});
    const EntityId upright = sketch.addLine({5.0, -5.0}, {5.0, 5.0});
    const EntityId shortOne = sketch.addLine({30.0, -5.0}, {30.0, 5.0});
    const EntityId circle = sketch.addCircle({10.0, 0.0}, 4.0);
    // The upper half of the same circle.
    const EntityId arc = sketch.addArc({10.0, 0.0}, {14.0, 0.0}, {6.0, 0.0});
    const EntityId other = sketch.addCircle({16.0, 0.0}, 4.0);

    std::vector<Point2> at = intersections(sketch, flat, upright);
    REQUIRE(at.size() == 1);
    CHECK_THAT(at[0].x, WithinAbs(5.0, lengthTolerance));
    CHECK_THAT(at[0].y, WithinAbs(0.0, lengthTolerance));
    // The lines would cross at (30, 0), which is beyond the end of `flat`.
    CHECK(intersections(sketch, flat, shortOne).empty());

    at = intersections(sketch, flat, circle);
    REQUIRE(at.size() == 2);
    CHECK_THAT(std::min(at[0].x, at[1].x), WithinAbs(6.0, lengthTolerance));
    CHECK_THAT(std::max(at[0].x, at[1].x), WithinAbs(14.0, lengthTolerance));
    // The upright line at x = 5 misses the circle, which starts at x = 6.
    CHECK(intersections(sketch, upright, circle).empty());

    at = intersections(sketch, circle, other);
    REQUIRE(at.size() == 2);
    CHECK_THAT(at[0].x, WithinAbs(13.0, lengthTolerance));
    CHECK_THAT(std::abs(at[0].y), WithinAbs(std::sqrt(7.0), lengthTolerance));
    // Only the upper of those two is on the arc.
    at = intersections(sketch, arc, other);
    REQUIRE(at.size() == 1);
    CHECK(at[0].y > 0.0);
    CHECK(intersections(sketch, flat, sketch.origin()).empty());
}

TEST_CASE("The rectangle tool makes four joined lines with four freedoms", "[sketch][edit]") {
    Sketch sketch;
    // Given by any two opposite corners.
    const RectangleParts r = addRectangle(sketch, {40.0, 5.0}, {10.0, 25.0});
    CHECK_THAT(startOf(sketch, r.bottom).x, WithinAbs(10.0, lengthTolerance));
    CHECK_THAT(startOf(sketch, r.bottom).y, WithinAbs(5.0, lengthTolerance));
    CHECK_THAT(endOf(sketch, r.right).x, WithinAbs(40.0, lengthTolerance));
    CHECK_THAT(endOf(sketch, r.right).y, WithinAbs(25.0, lengthTolerance));
    CHECK_THAT(lengthOf(sketch, r.top), WithinAbs(30.0, lengthTolerance));
    CHECK_THAT(lengthOf(sketch, r.left), WithinAbs(20.0, lengthTolerance));

    SolveResult state = sketch.solve();
    CHECK(state.status == SolveStatus::Solved);
    // Where it is, how wide, how tall.
    CHECK(state.degreesOfFreedom == 4);
    REQUIRE(sketch.profiles().size() == 1);
    CHECK_THAT(sketch.profiles()[0].area, WithinAbs(600.0, 1e-6));

    // Dragging a corner keeps it a rectangle.
    sketch.dragBy(sketch.find(r.top)->start, 7.0, 3.0);
    CHECK_THAT(startOf(sketch, r.top).y, WithinAbs(endOf(sketch, r.top).y, lengthTolerance));
    CHECK_THAT(startOf(sketch, r.right).x, WithinAbs(endOf(sketch, r.right).x, lengthTolerance));
    // A drag goes where it is pulled, near enough; it is not a dimension.
    CHECK_THAT(lengthOf(sketch, r.bottom), WithinAbs(37.0, 1e-3));

    const Sketch before = sketch;
    CHECK_THROWS_AS(addRectangle(sketch, {1.0, 1.0}, {1.0, 9.0}), std::invalid_argument);
    CHECK_THROWS_AS(addRectangle(sketch, {1.0, 1.0}, {9.0, 1.0}), std::invalid_argument);
    CHECK(same(sketch, before));
}

TEST_CASE("The polygon tool makes a regular polygon that stays regular", "[sketch][edit]") {
    Sketch sketch;
    const PolygonParts hexagon = addPolygon(sketch, {10.0, 10.0}, {30.0, 10.0}, 6);
    REQUIRE(hexagon.sides.size() == 6);
    CHECK(sketch.find(hexagon.circle)->construction);
    for (const EntityId side : hexagon.sides) {
        CHECK_THAT(lengthOf(sketch, side), WithinAbs(20.0, lengthTolerance));
    }
    SolveResult state = sketch.solve();
    CHECK(state.status == SolveStatus::Solved);
    // Where its center is, how large it is, and which way it is turned.
    CHECK(state.degreesOfFreedom == 4);
    REQUIRE(sketch.profiles().size() == 1);
    CHECK_THAT(sketch.profiles()[0].area, WithinAbs(1.5 * std::sqrt(3.0) * 400.0, 1e-6));

    // Pull one corner outward: the whole polygon grows and every side stays as long as the
    // others.
    sketch.dragBy(sketch.find(hexagon.sides[0])->start, 6.0, 0.0);
    const double side = lengthOf(sketch, hexagon.sides[0]);
    CHECK(side > 20.5);
    for (const EntityId each : hexagon.sides) {
        CHECK_THAT(lengthOf(sketch, each), WithinAbs(side, 1e-6));
        CHECK_THAT(apart(startOf(sketch, each), centerOf(sketch, hexagon.circle)),
                   WithinAbs(sketch.radiusOf(hexagon.circle), 1e-6));
    }

    CHECK(addPolygon(sketch, {0.0, 0.0}, {0.0, 5.0}, 3).sides.size() == 3);
    const Sketch before = sketch;
    CHECK_THROWS_AS(addPolygon(sketch, {0.0, 0.0}, {5.0, 0.0}, 2), std::invalid_argument);
    CHECK_THROWS_AS(addPolygon(sketch, {0.0, 0.0}, {5.0, 0.0}, 65), std::invalid_argument);
    CHECK_THROWS_AS(addPolygon(sketch, {3.0, 3.0}, {3.0, 3.0}, 5), std::invalid_argument);
    CHECK(same(sketch, before));
}

TEST_CASE("The slot tool makes a closed slot with five freedoms", "[sketch][edit]") {
    Sketch sketch;
    // Along a slanted axis, so that nothing lines up with X or Y by luck.
    const SlotParts slot = addSlot(sketch, {0.0, 0.0}, {30.0, 40.0}, 6.0);
    SolveResult state = sketch.solve();
    CHECK(state.status == SolveStatus::Solved);
    // Two centers and the radius.
    CHECK(state.degreesOfFreedom == 5);
    REQUIRE(sketch.profiles().size() == 1);
    CHECK_THAT(sketch.profiles()[0].area, WithinAbs(50.0 * 12.0 + pi * 36.0, 1e-6));
    CHECK_THAT(lengthOf(sketch, slot.firstLine), WithinAbs(50.0, lengthTolerance));
    CHECK_THAT(sketch.radiusOf(slot.secondArc), WithinAbs(6.0, lengthTolerance));

    // Move one center: still a slot, its lines still as far from the axis as the radius.
    sketch.dragBy(sketch.find(slot.secondArc)->center, 10.0, -15.0);
    const double radius = sketch.radiusOf(slot.firstArc);
    CHECK_THAT(sketch.radiusOf(slot.secondArc), WithinAbs(radius, 1e-6));
    const double between = apart(centerOf(sketch, slot.firstArc), centerOf(sketch, slot.secondArc));
    CHECK_THAT(lengthOf(sketch, slot.firstLine), WithinAbs(between, 1e-6));
    CHECK_THAT(lengthOf(sketch, slot.secondLine), WithinAbs(between, 1e-6));
    REQUIRE(sketch.profiles().size() == 1);
    CHECK_THAT(sketch.profiles()[0].area,
               WithinAbs(between * 2.0 * radius + pi * radius * radius, 1e-5));

    const Sketch before = sketch;
    CHECK_THROWS_AS(addSlot(sketch, {1.0, 1.0}, {1.0, 1.0}, 3.0), std::invalid_argument);
    CHECK_THROWS_AS(addSlot(sketch, {1.0, 1.0}, {9.0, 1.0}, 0.0), std::invalid_argument);
    CHECK(same(sketch, before));
}

TEST_CASE("A constraint tool works out what it means for the selection", "[sketch][edit]") {
    Sketch sketch;
    const EntityId line = sketch.addLine({0.0, 0.0}, {10.0, 1.0});
    const EntityId other = sketch.addLine({0.0, 5.0}, {10.0, 7.0});
    const EntityId circle = sketch.addCircle({20.0, 0.0}, 3.0);
    const EntityId arc = sketch.addArc({30.0, 0.0}, {33.0, 0.0}, {30.0, 3.0});
    const EntityId point = sketch.addPoint({5.0, 9.0});
    const EntityId lineStart = sketch.find(line)->start;
    const EntityId otherEnd = sketch.find(other)->end;

    const auto kindFor = [&sketch](ConstraintKind tool, const std::vector<EntityId>& selection) {
        return constraintFor(sketch, tool, selection);
    };
    using Kind = ConstraintKind;

    // Coincident: two points, or a point and whatever it should lie on, in either order.
    auto made = kindFor(Kind::Coincident, {lineStart, otherEnd});
    REQUIRE(made.has_value());
    CHECK(made->kind == Kind::Coincident);
    made = kindFor(Kind::Coincident, {other, point});
    REQUIRE(made.has_value());
    CHECK(made->kind == Kind::PointOnLine);
    CHECK(made->first == point);
    CHECK(made->second == other);
    made = kindFor(Kind::Coincident, {point, arc});
    REQUIRE(made.has_value());
    CHECK(made->kind == Kind::PointOnCircle);
    CHECK(made->second == arc);
    // A line's own end is on the line already.
    CHECK_FALSE(kindFor(Kind::Coincident, {lineStart, line}).has_value());
    CHECK_FALSE(kindFor(Kind::Coincident, {line, other}).has_value());
    CHECK_FALSE(kindFor(Kind::Coincident, {point}).has_value());

    CHECK(kindFor(Kind::Horizontal, {line}).has_value());
    made = kindFor(Kind::Vertical, {lineStart, point});
    REQUIRE(made.has_value());
    CHECK(made->second == point);
    CHECK_FALSE(kindFor(Kind::Horizontal, {circle}).has_value());
    CHECK_FALSE(kindFor(Kind::Horizontal, {line, other}).has_value());

    CHECK(kindFor(Kind::Parallel, {line, other}).has_value());
    CHECK(kindFor(Kind::Perpendicular, {line, other}).has_value());
    CHECK(kindFor(Kind::Collinear, {line, other}).has_value());
    CHECK_FALSE(kindFor(Kind::Parallel, {line, circle}).has_value());
    CHECK_FALSE(kindFor(Kind::Parallel, {line, line}).has_value());

    CHECK(kindFor(Kind::Tangent, {line, circle}).has_value());
    CHECK(kindFor(Kind::Tangent, {arc, line}).has_value());
    CHECK(kindFor(Kind::Tangent, {arc, circle}).has_value());
    CHECK_FALSE(kindFor(Kind::Tangent, {line, other}).has_value());

    CHECK(kindFor(Kind::Equal, {line, other}).has_value());
    CHECK(kindFor(Kind::Equal, {circle, arc}).has_value());
    CHECK_FALSE(kindFor(Kind::Equal, {line, circle}).has_value());
    CHECK(kindFor(Kind::Concentric, {circle, arc}).has_value());
    CHECK_FALSE(kindFor(Kind::Concentric, {circle, line}).has_value());

    made = kindFor(Kind::Midpoint, {other, point});
    REQUIRE(made.has_value());
    CHECK(made->first == point);
    CHECK(made->second == other);
    CHECK_FALSE(kindFor(Kind::Midpoint, {lineStart, line}).has_value());

    // Symmetric: two points and the line between them, picked in any order.
    made = kindFor(Kind::Symmetric, {line, point, otherEnd});
    REQUIRE(made.has_value());
    CHECK(made->first == point);
    CHECK(made->second == otherEnd);
    CHECK(made->third == line);
    CHECK_FALSE(kindFor(Kind::Symmetric, {line, other, point}).has_value());

    CHECK(kindFor(Kind::Fix, {point}).has_value());
    // The origin is fixed as it is.
    CHECK_FALSE(kindFor(Kind::Fix, {sketch.origin()}).has_value());
    CHECK_FALSE(kindFor(Kind::Fix, {line}).has_value());
    CHECK_FALSE(kindFor(Kind::Parallel, {line, 9999}).has_value());

    // Whatever constraintFor offers, the sketch accepts.
    for (const auto& [tool, selection] : std::vector<std::pair<Kind, std::vector<EntityId>>>{
             {Kind::Coincident, {other, point}},
             {Kind::Tangent, {arc, line}},
             {Kind::Symmetric, {line, point, otherEnd}},
             {Kind::Midpoint, {other, point}}}) {
        const auto choice = kindFor(tool, selection);
        REQUIRE(choice.has_value());
        Sketch copy = sketch;
        CHECK_NOTHROW(
            copy.addConstraint(choice->kind, choice->first, choice->second, choice->third));
    }
}

TEST_CASE("The dimension tool measures what the selection calls for", "[sketch][edit]") {
    Sketch sketch;
    const EntityId line = sketch.addLine({0.0, 0.0}, {30.0, 40.0});
    const EntityId level = sketch.addLine({0.0, -10.0}, {20.0, -10.0});
    const EntityId alongside = sketch.addLine({5.0, -17.0}, {50.0, -17.0});
    const EntityId circle = sketch.addCircle({60.0, 0.0}, 4.0);
    const EntityId arc = sketch.addArc({80.0, 0.0}, {83.0, 0.0}, {80.0, 3.0});
    const EntityId point = sketch.addPoint({10.0, 5.0});
    using Kind = ConstraintKind;

    auto made = dimensionFor(sketch, {line});
    REQUIRE(made.has_value());
    CHECK(made->kind == Kind::Distance);
    CHECK(made->first == sketch.find(line)->start);
    CHECK(made->second == sketch.find(line)->end);
    CHECK_THAT(made->value, WithinAbs(50.0, lengthTolerance));

    made = dimensionFor(sketch, {circle});
    REQUIRE(made.has_value());
    CHECK(made->kind == Kind::Diameter);
    CHECK_THAT(made->value, WithinAbs(8.0, lengthTolerance));

    made = dimensionFor(sketch, {arc});
    REQUIRE(made.has_value());
    CHECK(made->kind == Kind::Radius);
    CHECK_THAT(made->value, WithinAbs(3.0, lengthTolerance));

    made = dimensionFor(sketch, {point, sketch.origin()});
    REQUIRE(made.has_value());
    CHECK(made->kind == Kind::Distance);
    CHECK_THAT(made->value, WithinAbs(std::hypot(10.0, 5.0), lengthTolerance));

    made = dimensionFor(sketch, {level, point});
    REQUIRE(made.has_value());
    CHECK(made->kind == Kind::PointLineDistance);
    CHECK(made->first == point);
    CHECK_THAT(made->value, WithinAbs(15.0, lengthTolerance));

    // Two lines: the angle between them, unless they are parallel.
    made = dimensionFor(sketch, {level, line});
    REQUIRE(made.has_value());
    CHECK(made->kind == Kind::Angle);
    CHECK_THAT(made->value, WithinAbs(std::atan2(40.0, 30.0), 1e-9));
    made = dimensionFor(sketch, {level, alongside});
    REQUIRE(made.has_value());
    CHECK(made->kind == Kind::PointLineDistance);
    CHECK_THAT(made->value, WithinAbs(7.0, lengthTolerance));

    CHECK_FALSE(dimensionFor(sketch, {}).has_value());
    CHECK_FALSE(dimensionFor(sketch, {point}).has_value());
    CHECK_FALSE(dimensionFor(sketch, {circle, arc}).has_value());
    CHECK_FALSE(dimensionFor(sketch, {sketch.find(line)->start, line}).has_value());
    CHECK_FALSE(dimensionFor(sketch, {line, level, point}).has_value());
    // A dimension tool asked through constraintFor gives the same answer.
    CHECK(constraintFor(sketch, Kind::Distance, {circle})->kind == Kind::Diameter);
}

TEST_CASE("A constraint that adds nothing or cannot hold is refused", "[sketch][edit]") {
    Sketch sketch;
    const RectangleParts r = addRectangle(sketch, {0.0, 0.0}, {40.0, 25.0});
    sketch.addConstraint(ConstraintKind::Coincident, sketch.find(r.bottom)->start, sketch.origin());
    const EntityId bottomStart = sketch.find(r.bottom)->start;
    const EntityId bottomEnd = sketch.find(r.bottom)->end;

    Outcome outcome =
        tryConstraint(sketch, ConstraintKind::Distance, bottomStart, bottomEnd, 0, 60.0);
    REQUIRE(outcome.ok());
    CHECK(outcome.constraint != 0);
    CHECK(sketch.findConstraint(outcome.constraint) != nullptr);
    CHECK(outcome.result.degreesOfFreedom == 1);
    // It was solved as well as added.
    CHECK_THAT(lengthOf(sketch, r.top), WithinAbs(60.0, lengthTolerance));

    const Sketch before = sketch;
    // The sides of a rectangle are parallel already.
    outcome = tryConstraint(sketch, ConstraintKind::Parallel, r.bottom, r.top);
    CHECK(outcome.refusal == Refusal::Redundant);
    CHECK(outcome.constraint == 0);
    CHECK(same(sketch, before));
    // And they cannot be square to each other.
    outcome = tryConstraint(sketch, ConstraintKind::Perpendicular, r.bottom, r.top);
    CHECK(outcome.refusal == Refusal::Conflicting);
    CHECK(same(sketch, before));
    // The top is as long as the bottom, which has its length.
    outcome = tryConstraint(sketch, ConstraintKind::Distance, sketch.find(r.top)->start,
                            sketch.find(r.top)->end, 0, 45.0);
    CHECK(outcome.refusal == Refusal::Conflicting);
    CHECK(same(sketch, before));
    // Not something a tangent applies to.
    outcome = tryConstraint(sketch, ConstraintKind::Tangent, r.bottom, r.top);
    CHECK(outcome.refusal == Refusal::Invalid);
    outcome = tryConstraint(sketch, ConstraintKind::Distance, bottomStart, bottomEnd, 0, -3.0);
    CHECK(outcome.refusal == Refusal::Invalid);
    CHECK(same(sketch, before));
    CHECK(outcome.result.degreesOfFreedom == 1);
}

TEST_CASE("A dimension value the geometry cannot follow is refused", "[sketch][edit]") {
    Sketch sketch;
    // A triangle with all three sides dimensioned.
    const EntityId a = sketch.addLine({0.0, 0.0}, {30.0, 0.0});
    const EntityId b = sketch.addLine({30.0, 0.0}, {0.0, 40.0});
    const EntityId c = sketch.addLine({0.0, 40.0}, {0.0, 0.0});
    const auto join = [&sketch](EntityId first, EntityId second) {
        sketch.addConstraint(ConstraintKind::Coincident, sketch.find(first)->end,
                             sketch.find(second)->start);
    };
    join(a, b);
    join(b, c);
    join(c, a);
    const auto sideLength = [&sketch](EntityId line, double value) {
        return sketch.addConstraint(ConstraintKind::Distance, sketch.find(line)->start,
                                    sketch.find(line)->end, 0, value);
    };
    sideLength(a, 30.0);
    const ConstraintId hypotenuse = sideLength(b, 50.0);
    sideLength(c, 40.0);
    REQUIRE(sketch.solve().status == SolveStatus::Solved);

    Outcome outcome = tryDimensionValue(sketch, hypotenuse, 60.0);
    REQUIRE(outcome.ok());
    CHECK_THAT(lengthOf(sketch, b), WithinAbs(60.0, lengthTolerance));
    CHECK_THAT(lengthOf(sketch, a), WithinAbs(30.0, lengthTolerance));

    // Sides of 30 and 40 cannot reach across 80.
    const Sketch before = sketch;
    outcome = tryDimensionValue(sketch, hypotenuse, 80.0);
    CHECK(outcome.refusal == Refusal::Conflicting);
    CHECK(same(sketch, before));
    CHECK(sketch.findConstraint(hypotenuse)->value == 60.0);
    outcome = tryDimensionValue(sketch, hypotenuse, 0.0);
    CHECK(outcome.refusal == Refusal::Invalid);
    outcome = tryDimensionValue(sketch, 9999, 5.0);
    CHECK(outcome.refusal == Refusal::Invalid);
    CHECK(same(sketch, before));
}

TEST_CASE("Mirror images follow their originals", "[sketch][edit]") {
    Sketch sketch;
    // The axis is the vertical line x = 10.
    const EntityId axis = sketch.addLine({10.0, -20.0}, {10.0, 20.0});
    sketch.setConstruction(axis, true);
    sketch.addConstraint(ConstraintKind::Fix, sketch.find(axis)->start);
    sketch.addConstraint(ConstraintKind::Fix, sketch.find(axis)->end);
    const EntityId line = sketch.addLine({0.0, 0.0}, {6.0, 8.0});
    const EntityId circle = sketch.addCircle({2.0, -10.0}, 3.0);
    const EntityId arc = sketch.addArc({0.0, 12.0}, {4.0, 12.0}, {0.0, 16.0});
    const EntityId point = sketch.addPoint({7.0, 1.0});
    sketch.setConstruction(circle, true);
    const int before = sketch.solve().degreesOfFreedom;

    const std::vector<EntityId> made = mirror(sketch, {line, circle, arc, point}, axis);
    REQUIRE(made.size() == 4);
    CHECK_THAT(startOf(sketch, made[0]).x, WithinAbs(20.0, lengthTolerance));
    CHECK_THAT(endOf(sketch, made[0]).x, WithinAbs(14.0, lengthTolerance));
    CHECK_THAT(endOf(sketch, made[0]).y, WithinAbs(8.0, lengthTolerance));
    CHECK_THAT(centerOf(sketch, made[1]).x, WithinAbs(18.0, lengthTolerance));
    CHECK_THAT(sketch.radiusOf(made[1]), WithinAbs(3.0, lengthTolerance));
    CHECK(sketch.find(made[1])->construction);
    CHECK_FALSE(sketch.find(made[0])->construction);
    CHECK_THAT(sketch.position(made[3]).x, WithinAbs(13.0, lengthTolerance));

    // The image of a quarter arc is a quarter arc, not the other three quarters: it starts
    // where the original's end went.
    CHECK_THAT(startOf(sketch, made[2]).x, WithinAbs(20.0, lengthTolerance));
    CHECK_THAT(startOf(sketch, made[2]).y, WithinAbs(16.0, lengthTolerance));
    CHECK_THAT(endOf(sketch, made[2]).x, WithinAbs(16.0, lengthTolerance));
    CHECK_THAT(endOf(sketch, made[2]).y, WithinAbs(12.0, lengthTolerance));
    // Its middle is at the mirror image of the original's middle.
    const double s = std::sqrt(0.5) * 4.0;
    CHECK_THAT(distanceTo(sketch, made[2], {20.0 - s, 12.0 + s}), WithinAbs(0.0, 1e-6));

    // The images add no freedom: they go wherever the originals go.
    const SolveResult state = sketch.solve();
    CHECK(state.status == SolveStatus::Solved);
    CHECK(state.degreesOfFreedom == before);
    sketch.dragBy(sketch.find(line)->end, -2.0, 5.0);
    CHECK_THAT(endOf(sketch, made[0]).x, WithinAbs(20.0 - endOf(sketch, line).x, 1e-6));
    CHECK_THAT(endOf(sketch, made[0]).y, WithinAbs(endOf(sketch, line).y, 1e-6));
    sketch.dragRadius(circle, {2.0, -4.0});
    CHECK_THAT(sketch.radiusOf(made[1]), WithinAbs(sketch.radiusOf(circle), 1e-6));
    CHECK(sketch.radiusOf(circle) > 5.0);

    const Sketch unchanged = sketch;
    CHECK_THROWS_AS(mirror(sketch, {line}, circle), std::invalid_argument);
    CHECK_THROWS_AS(mirror(sketch, {line, axis}, axis), std::invalid_argument);
    CHECK_THROWS_AS(mirror(sketch, {sketch.find(line)->start}, axis), std::invalid_argument);
    CHECK(same(sketch, unchanged));
}

TEST_CASE("Offset copies an outline at a distance, joined at its corners", "[sketch][edit]") {
    Sketch sketch;

    SECTION("a rectangle, outward and inward") {
        const RectangleParts r = addRectangle(sketch, {0.0, 0.0}, {40.0, 25.0});
        const std::vector<EntityId> sides{r.bottom, r.right, r.top, r.left};

        Sketch outward = sketch;
        std::vector<EntityId> made = offset(outward, sides, 5.0, {20.0, -30.0});
        REQUIRE(made.size() == 4);
        std::vector<Profile> profiles = outward.profiles();
        REQUIRE(profiles.size() == 2);
        CHECK_THAT(std::max(profiles[0].area, profiles[1].area), WithinAbs(50.0 * 35.0, 1e-6));
        // The copy of the bottom line lies 5 below it and is as much longer as the corners
        // need.
        CHECK_THAT(startOf(outward, made[0]).y, WithinAbs(-5.0, lengthTolerance));
        CHECK_THAT(startOf(outward, made[0]).x, WithinAbs(-5.0, lengthTolerance));
        CHECK_THAT(endOf(outward, made[0]).x, WithinAbs(45.0, lengthTolerance));
        CHECK(hasConstraint(outward, ConstraintKind::Parallel, r.bottom, made[0]));
        CHECK(outward.solve().status == SolveStatus::Solved);

        Sketch inward = sketch;
        made = offset(inward, sides, 5.0, {20.0, 12.0});
        profiles = inward.profiles();
        REQUIRE(profiles.size() == 2);
        CHECK_THAT(std::min(profiles[0].area, profiles[1].area), WithinAbs(30.0 * 15.0, 1e-6));

        // The distance is a dimension on the first line: change it and that line follows.
        // (The other three keep their own distances; there is no constraint yet that says
        // "the same distance all round".)
        const auto dimension = std::find_if(
            inward.constraints().begin(), inward.constraints().end(),
            [](const Constraint& c) { return c.kind == ConstraintKind::PointLineDistance; });
        REQUIRE(dimension != inward.constraints().end());
        CHECK(dimension->value == 5.0);
        REQUIRE(tryDimensionValue(inward, dimension->id, 8.0).ok());
        CHECK_THAT(startOf(inward, made[0]).y - startOf(inward, r.bottom).y,
                   WithinAbs(8.0, lengthTolerance));
    }

    SECTION("one line, to the side asked for") {
        const EntityId line = sketch.addLine({0.0, 0.0}, {10.0, 10.0});
        const std::vector<EntityId> left = offset(sketch, {line}, std::sqrt(2.0), {0.0, 5.0});
        REQUIRE(left.size() == 1);
        CHECK_THAT(startOf(sketch, left[0]).x, WithinAbs(-1.0, lengthTolerance));
        CHECK_THAT(startOf(sketch, left[0]).y, WithinAbs(1.0, lengthTolerance));
        CHECK_THAT(endOf(sketch, left[0]).x, WithinAbs(9.0, lengthTolerance));
        const std::vector<EntityId> right = offset(sketch, {line}, std::sqrt(2.0), {5.0, 0.0});
        CHECK_THAT(startOf(sketch, right[0]).x, WithinAbs(1.0, lengthTolerance));
        CHECK_THAT(startOf(sketch, right[0]).y, WithinAbs(-1.0, lengthTolerance));
    }

    SECTION("a slot: lines and arcs together") {
        const SlotParts slot = addSlot(sketch, {0.0, 0.0}, {30.0, 0.0}, 6.0);
        const std::vector<EntityId> curves{slot.firstArc, slot.firstLine, slot.secondArc,
                                           slot.secondLine};
        Sketch outward = sketch;
        std::vector<EntityId> made = offset(outward, curves, 2.0, {15.0, 20.0});
        REQUIRE(made.size() == 4);
        std::vector<Profile> profiles = outward.profiles();
        REQUIRE(profiles.size() == 2);
        CHECK_THAT(std::max(profiles[0].area, profiles[1].area),
                   WithinAbs(30.0 * 16.0 + pi * 64.0, 1e-6));
        CHECK(outward.solve().status == SolveStatus::Solved);

        Sketch inward = sketch;
        made = offset(inward, curves, 2.0, {15.0, 0.0});
        profiles = inward.profiles();
        REQUIRE(profiles.size() == 2);
        CHECK_THAT(std::min(profiles[0].area, profiles[1].area),
                   WithinAbs(30.0 * 8.0 + pi * 16.0, 1e-6));
        // More than the radius leaves nothing of the arcs.
        const Sketch before = inward;
        CHECK_THROWS_AS(offset(inward, curves, 6.0, {15.0, 0.0}), std::invalid_argument);
        CHECK(same(inward, before));
    }

    SECTION("a circle, and a corner between two lines") {
        const EntityId circle = sketch.addCircle({0.0, 0.0}, 10.0);
        std::vector<EntityId> made = offset(sketch, {circle}, 3.0, {0.0, 20.0});
        CHECK_THAT(sketch.radiusOf(made[0]), WithinAbs(13.0, lengthTolerance));
        made = offset(sketch, {circle}, 3.0, {1.0, 1.0});
        CHECK_THAT(sketch.radiusOf(made[0]), WithinAbs(7.0, lengthTolerance));
        CHECK(hasConstraint(sketch, ConstraintKind::Concentric, circle, made[0]));

        // An L drawn with the second line running back toward the corner, so the outline
        // runs against it.
        const EntityId flat = sketch.addLine({100.0, 0.0}, {120.0, 0.0});
        const EntityId up = sketch.addLine({120.0, 15.0}, {120.0, 0.0});
        // The side is given next to that second line, so it is the one that decides.
        made = offset(sketch, {flat, up}, 4.0, {117.0, 10.0});
        REQUIRE(made.size() == 2);
        // Inside the L: the corner moves to (116, 4), and both copies end there.
        CHECK_THAT(endOf(sketch, made[0]).x, WithinAbs(116.0, lengthTolerance));
        CHECK_THAT(endOf(sketch, made[0]).y, WithinAbs(4.0, lengthTolerance));
        CHECK_THAT(endOf(sketch, made[1]).x, WithinAbs(116.0, lengthTolerance));
        CHECK_THAT(endOf(sketch, made[1]).y, WithinAbs(4.0, lengthTolerance));
        CHECK_THAT(startOf(sketch, made[1]).y, WithinAbs(15.0, lengthTolerance));
        CHECK(hasConstraint(sketch, ConstraintKind::Coincident, sketch.find(made[0])->end,
                            sketch.find(made[1])->end));
    }

    SECTION("input that makes no sense is refused") {
        const EntityId line = sketch.addLine({0.0, 0.0}, {10.0, 0.0});
        const Sketch before = sketch;
        CHECK_THROWS_AS(offset(sketch, {line}, 0.0, {0.0, 5.0}), std::invalid_argument);
        CHECK_THROWS_AS(offset(sketch, {line}, -2.0, {0.0, 5.0}), std::invalid_argument);
        CHECK_THROWS_AS(offset(sketch, {sketch.origin()}, 2.0, {0.0, 5.0}), std::invalid_argument);
        CHECK(same(sketch, before));
    }
}

TEST_CASE("Trim removes the part of a curve between its crossings", "[sketch][edit]") {
    Sketch sketch;
    // A long horizontal line crossed by two upright ones at x = 10 and x = 30.
    const EntityId flat = sketch.addLine({0.0, 0.0}, {40.0, 0.0});
    const EntityId first = sketch.addLine({10.0, -5.0}, {10.0, 5.0});
    const EntityId second = sketch.addLine({30.0, -5.0}, {30.0, 5.0});

    SECTION("the middle goes, leaving two lines held on the cutting lines") {
        // Something attached to the far end has to stay attached to it.
        const EntityId beyond = sketch.addLine({40.0, 0.0}, {50.0, 10.0});
        sketch.addConstraint(ConstraintKind::Coincident, sketch.find(flat)->end,
                             sketch.find(beyond)->start);
        REQUIRE(trim(sketch, flat, {20.0, 0.5}));
        CHECK(curveCount(sketch) == 5);
        CHECK_THAT(startOf(sketch, flat).x, WithinAbs(0.0, lengthTolerance));
        CHECK_THAT(endOf(sketch, flat).x, WithinAbs(10.0, lengthTolerance));
        // The other piece is the newest curve.
        EntityId rest = 0;
        for (const Entity& entity : sketch.entities()) {
            if (entity.kind == EntityKind::Line && entity.id > beyond) {
                rest = entity.id;
            }
        }
        REQUIRE(rest != 0);
        CHECK_THAT(startOf(sketch, rest).x, WithinAbs(30.0, lengthTolerance));
        CHECK_THAT(endOf(sketch, rest).x, WithinAbs(40.0, lengthTolerance));
        CHECK(hasConstraint(sketch, ConstraintKind::PointOnLine, sketch.find(flat)->end, first));
        CHECK(hasConstraint(sketch, ConstraintKind::PointOnLine, sketch.find(rest)->start, second));
        CHECK(hasConstraint(sketch, ConstraintKind::Coincident, sketch.find(rest)->end,
                            sketch.find(beyond)->start));
        CHECK(sketch.solve().status == SolveStatus::Solved);

        // Move a cutting line: the cut end goes with it.
        sketch.dragBy(first, 3.0, 0.0);
        CHECK_THAT(endOf(sketch, flat).x, WithinAbs(startOf(sketch, first).x, 1e-6));
    }

    SECTION("an end goes, back to the nearest crossing") {
        REQUIRE(trim(sketch, flat, {36.0, -0.2}));
        CHECK(curveCount(sketch) == 3);
        CHECK_THAT(endOf(sketch, flat).x, WithinAbs(30.0, lengthTolerance));
        CHECK_THAT(startOf(sketch, flat).x, WithinAbs(0.0, lengthTolerance));
        REQUIRE(trim(sketch, flat, {2.0, 0.0}));
        CHECK_THAT(startOf(sketch, flat).x, WithinAbs(10.0, lengthTolerance));
        CHECK(hasConstraint(sketch, ConstraintKind::PointOnLine, sketch.find(flat)->start, first));
        // What is left has nothing crossing it between its ends, so trimming removes it.
        REQUIRE(trim(sketch, flat, {20.0, 0.0}));
        CHECK(sketch.find(flat) == nullptr);
        CHECK(curveCount(sketch) == 2);
    }

    SECTION("a length dimension on the trimmed line does not pull it back") {
        sketch.addConstraint(ConstraintKind::Distance, sketch.find(flat)->start,
                             sketch.find(flat)->end, 0, 40.0);
        REQUIRE(trim(sketch, flat, {36.0, 0.0}));
        CHECK(sketch.solve().status == SolveStatus::Solved);
        CHECK_THAT(lengthOf(sketch, flat), WithinAbs(30.0, lengthTolerance));
    }

    SECTION("a circle becomes the arc on the side that was not clicked") {
        const EntityId circle = sketch.addCircle({20.0, 0.0}, 5.0);
        // The flat line crosses it at x = 15 and x = 25. Click its upper half.
        REQUIRE(trim(sketch, circle, {20.0, 5.2}));
        REQUIRE(sketch.find(circle) != nullptr);
        CHECK(sketch.find(circle)->kind == EntityKind::Arc);
        CHECK_THAT(sketch.radiusOf(circle), WithinAbs(5.0, lengthTolerance));
        // The lower half is left: counter-clockwise from (15, 0) to (25, 0).
        CHECK_THAT(startOf(sketch, circle).x, WithinAbs(15.0, lengthTolerance));
        CHECK_THAT(endOf(sketch, circle).x, WithinAbs(25.0, lengthTolerance));
        CHECK_THAT(distanceTo(sketch, circle, {20.0, -5.0}), WithinAbs(0.0, 1e-6));
        CHECK(distanceTo(sketch, circle, {20.0, 5.0}) > 4.0);
        CHECK(hasConstraint(sketch, ConstraintKind::PointOnLine, sketch.find(circle)->start, flat));
        CHECK(sketch.solve().status == SolveStatus::Solved);
    }

    SECTION("an arc is cut like a line") {
        // The upper half circle around (20, -2), radius 12: the upright lines cross it.
        const EntityId arc = sketch.addArc({20.0, -2.0}, {32.0, -2.0}, {8.0, -2.0});
        const double before = sketch.radiusOf(arc);
        REQUIRE(trim(sketch, arc, {20.0, 10.0}));
        // Two arcs now, both still on the same circle.
        int arcs = 0;
        for (const Entity& entity : sketch.entities()) {
            if (entity.kind == EntityKind::Arc) {
                ++arcs;
                CHECK_THAT(sketch.radiusOf(entity.id), WithinAbs(before, 1e-6));
                CHECK_THAT(centerOf(sketch, entity.id).x, WithinAbs(20.0, 1e-6));
                CHECK(distanceTo(sketch, entity.id, {20.0, 10.0}) > 5.0);
            }
        }
        CHECK(arcs == 2);
        CHECK_THAT(endOf(sketch, arc).x, WithinAbs(30.0, lengthTolerance));
        CHECK(sketch.solve().status == SolveStatus::Solved);
    }

    SECTION("there has to be something to trim") {
        const Sketch before = sketch;
        CHECK_FALSE(trim(sketch, sketch.origin(), {0.0, 0.0}));
        CHECK_FALSE(trim(sketch, 9999, {0.0, 0.0}));
        CHECK(same(sketch, before));
        // A circle crossed in one place only cannot be opened.
        const EntityId touching = sketch.addCircle({45.0, 0.0}, 5.0);
        const Sketch withCircle = sketch;
        CHECK_FALSE(trim(sketch, touching, {45.0, 5.0}));
        CHECK(same(sketch, withCircle));
    }
}

TEST_CASE("Extend lengthens a curve to the next one it meets", "[sketch][edit]") {
    Sketch sketch;
    const EntityId wall = sketch.addLine({30.0, -20.0}, {30.0, 20.0});
    const EntityId farWall = sketch.addLine({50.0, -20.0}, {50.0, 20.0});
    const EntityId line = sketch.addLine({0.0, 0.0}, {10.0, 5.0});

    SECTION("a line, at the end nearer to the click") {
        REQUIRE(extend(sketch, line, {9.0, 5.0}));
        // To the nearer wall, not the farther one.
        CHECK_THAT(endOf(sketch, line).x, WithinAbs(30.0, lengthTolerance));
        CHECK_THAT(endOf(sketch, line).y, WithinAbs(15.0, lengthTolerance));
        CHECK_THAT(startOf(sketch, line).x, WithinAbs(0.0, lengthTolerance));
        CHECK(hasConstraint(sketch, ConstraintKind::PointOnLine, sketch.find(line)->end, wall));
        CHECK(sketch.solve().status == SolveStatus::Solved);
        // Again: on to the far wall? No: it would leave the wall's extent (y = 25 at x = 50).
        const Sketch before = sketch;
        CHECK_FALSE(extend(sketch, line, {29.0, 15.0}));
        CHECK(same(sketch, before));
        // And nothing lies beyond its start.
        CHECK_FALSE(extend(sketch, line, {0.0, 0.0}));
        CHECK(farWall != 0);
    }

    SECTION("an arc, around its circle") {
        // A quarter arc around (30, 0) with radius 10, from (40, 0) up to (30, 10): its end is
        // on the wall already, its start is not on anything.
        const EntityId arc = sketch.addArc({35.0, 0.0}, {45.0, 0.0}, {35.0, 10.0});
        // Counter-clockwise from the end it meets the wall at x = 30.
        REQUIRE(extend(sketch, arc, {35.0, 10.0}));
        CHECK_THAT(endOf(sketch, arc).x, WithinAbs(30.0, lengthTolerance));
        CHECK_THAT(endOf(sketch, arc).y, WithinAbs(std::sqrt(75.0), lengthTolerance));
        CHECK_THAT(sketch.radiusOf(arc), WithinAbs(10.0, lengthTolerance));
        CHECK(hasConstraint(sketch, ConstraintKind::PointOnLine, sketch.find(arc)->end, wall));
        // Clockwise from the start it meets the same wall below.
        REQUIRE(extend(sketch, arc, {45.0, 0.0}));
        CHECK_THAT(startOf(sketch, arc).x, WithinAbs(30.0, lengthTolerance));
        CHECK_THAT(startOf(sketch, arc).y, WithinAbs(-std::sqrt(75.0), lengthTolerance));
        CHECK(sketch.solve().status == SolveStatus::Solved);
    }

    SECTION("a circle has no end to extend") {
        const EntityId circle = sketch.addCircle({0.0, 0.0}, 5.0);
        CHECK_FALSE(extend(sketch, circle, {5.0, 0.0}));
        CHECK_FALSE(extend(sketch, sketch.origin(), {0.0, 0.0}));
    }
}
