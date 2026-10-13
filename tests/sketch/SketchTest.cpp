// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/sketch/Sketch.h>

#include <Tolerances.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace lcad::sketch;
using Catch::Matchers::WithinAbs;
using lcad::test::lengthTolerance;

TEST_CASE("A new sketch holds its origin, a fixed point at zero", "[sketch]") {
    const Sketch sketch;

    REQUIRE(sketch.entities().size() == 1);
    const Entity* origin = sketch.find(sketch.origin());
    REQUIRE(origin != nullptr);
    CHECK(origin->kind == EntityKind::Point);
    CHECK(origin->fixed);
    CHECK(sketch.position(sketch.origin()).x == 0.0);
    CHECK(sketch.position(sketch.origin()).y == 0.0);
    CHECK(sketch.constraints().empty());
}

TEST_CASE("Curves come with the points that define them", "[sketch]") {
    Sketch sketch;

    const EntityId line = sketch.addLine({1.0, 2.0}, {4.0, 6.0});
    const EntityId circle = sketch.addCircle({10.0, 0.0}, 3.0);
    const EntityId arc = sketch.addArc({0.0, 0.0}, {5.0, 0.0}, {0.0, 9.0});
    const EntityId point = sketch.addPoint({7.0, 7.0});

    const Entity* l = sketch.find(line);
    REQUIRE(l != nullptr);
    CHECK(l->kind == EntityKind::Line);
    CHECK(sketch.find(l->start)->owner == line);
    CHECK(sketch.position(l->start).x == 1.0);
    CHECK(sketch.position(l->end).y == 6.0);

    const Entity* c = sketch.find(circle);
    CHECK(c->radius == 3.0);
    CHECK(sketch.position(c->center).x == 10.0);
    CHECK(sketch.radiusOf(circle) == 3.0);

    // The arc's end is put on the circle through its start.
    const Entity* a = sketch.find(arc);
    CHECK_THAT(sketch.radiusOf(arc), WithinAbs(5.0, lengthTolerance));
    CHECK_THAT(sketch.position(a->end).x, WithinAbs(0.0, lengthTolerance));
    CHECK_THAT(sketch.position(a->end).y, WithinAbs(5.0, lengthTolerance));

    CHECK(sketch.find(point)->owner == 0);
    // Origin, line + 2, circle + 1, arc + 3, point.
    CHECK(sketch.entities().size() == 1 + 3 + 2 + 4 + 1);
}

TEST_CASE("Geometry that describes nothing is refused", "[sketch]") {
    Sketch sketch;
    const double nan = std::numeric_limits<double>::quiet_NaN();

    CHECK_THROWS_AS(sketch.addLine({1.0, 1.0}, {1.0, 1.0}), std::invalid_argument);
    CHECK_THROWS_AS(sketch.addLine({nan, 1.0}, {2.0, 1.0}), std::invalid_argument);
    CHECK_THROWS_AS(sketch.addCircle({0.0, 0.0}, 0.0), std::invalid_argument);
    CHECK_THROWS_AS(sketch.addCircle({0.0, 0.0}, -2.0), std::invalid_argument);
    CHECK_THROWS_AS(sketch.addArc({0.0, 0.0}, {0.0, 0.0}, {1.0, 0.0}), std::invalid_argument);
    CHECK_THROWS_AS(sketch.addArc({0.0, 0.0}, {2.0, 0.0}, {4.0, 0.0}), std::invalid_argument);
    CHECK_THROWS_AS(sketch.addPoint({1.0, std::numeric_limits<double>::infinity()}),
                    std::invalid_argument);
    CHECK(sketch.entities().size() == 1);
}

TEST_CASE("Removing a curve takes its points and its constraints with it", "[sketch]") {
    Sketch sketch;
    const EntityId first = sketch.addLine({0.0, 0.0}, {10.0, 0.0});
    const EntityId second = sketch.addLine({10.0, 0.0}, {10.0, 5.0});
    const EntityId firstEnd = sketch.find(first)->end;
    const EntityId secondStart = sketch.find(second)->start;
    sketch.addConstraint(ConstraintKind::Coincident, firstEnd, secondStart);
    sketch.addConstraint(ConstraintKind::Horizontal, first);
    const ConstraintId vertical = sketch.addConstraint(ConstraintKind::Vertical, second);

    SECTION("by the curve") {
        sketch.remove(first);
    }
    SECTION("by one of its points") {
        sketch.remove(firstEnd);
    }

    CHECK(sketch.find(first) == nullptr);
    CHECK(sketch.find(firstEnd) == nullptr);
    CHECK(sketch.find(second) != nullptr);
    REQUIRE(sketch.constraints().size() == 1);
    CHECK(sketch.constraints()[0].id == vertical);
    // Origin, and the second line with its two points.
    CHECK(sketch.entities().size() == 4);
}

TEST_CASE("The origin stays, and ids are not reused", "[sketch]") {
    Sketch sketch;
    CHECK_THROWS_AS(sketch.remove(sketch.origin()), std::invalid_argument);
    CHECK_THROWS_AS(sketch.remove(999), std::invalid_argument);

    const EntityId first = sketch.addPoint({1.0, 1.0});
    sketch.remove(first);
    const EntityId second = sketch.addPoint({1.0, 1.0});
    CHECK(second != first);
}

TEST_CASE("Constraints are checked against the kinds of entity they take", "[sketch]") {
    Sketch sketch;
    const EntityId line = sketch.addLine({0.0, 0.0}, {10.0, 0.0});
    const EntityId other = sketch.addLine({0.0, 5.0}, {10.0, 6.0});
    const EntityId circle = sketch.addCircle({0.0, 0.0}, 3.0);
    const EntityId point = sketch.addPoint({1.0, 1.0});
    const EntityId start = sketch.find(line)->start;

    // What fits.
    CHECK(sketch.addConstraint(ConstraintKind::Parallel, line, other) != 0);
    CHECK(sketch.addConstraint(ConstraintKind::Tangent, circle, line) != 0);
    CHECK(sketch.addConstraint(ConstraintKind::PointOnCircle, point, circle) != 0);
    CHECK(sketch.addConstraint(ConstraintKind::Horizontal, start, point) != 0);
    CHECK(sketch.addConstraint(ConstraintKind::Distance, start, point, 0, 12.5) != 0);

    // What does not.
    CHECK_THROWS_AS(sketch.addConstraint(ConstraintKind::Parallel, line, circle),
                    std::invalid_argument);
    CHECK_THROWS_AS(sketch.addConstraint(ConstraintKind::Parallel, line, line),
                    std::invalid_argument);
    CHECK_THROWS_AS(sketch.addConstraint(ConstraintKind::Coincident, point, line),
                    std::invalid_argument);
    CHECK_THROWS_AS(sketch.addConstraint(ConstraintKind::Radius, line, 0, 0, 5.0),
                    std::invalid_argument);
    CHECK_THROWS_AS(sketch.addConstraint(ConstraintKind::Symmetric, start, point, circle),
                    std::invalid_argument);
    CHECK_THROWS_AS(sketch.addConstraint(ConstraintKind::Horizontal, 999), std::invalid_argument);
    // Dimensions need a usable value.
    CHECK_THROWS_AS(sketch.addConstraint(ConstraintKind::Distance, start, point, 0, 0.0),
                    std::invalid_argument);
    CHECK_THROWS_AS(sketch.addConstraint(ConstraintKind::Radius, circle, 0, 0, -1.0),
                    std::invalid_argument);
    CHECK(sketch.constraints().size() == 5);
}

TEST_CASE("A dimension's value can be changed, and other constraints have none", "[sketch]") {
    Sketch sketch;
    const EntityId line = sketch.addLine({0.0, 0.0}, {10.0, 0.0});
    const Entity* l = sketch.find(line);
    const ConstraintId length =
        sketch.addConstraint(ConstraintKind::Distance, l->start, l->end, 0, 10.0);
    const ConstraintId level = sketch.addConstraint(ConstraintKind::Horizontal, line);

    sketch.setConstraintValue(length, 25.0);
    CHECK(sketch.findConstraint(length)->value == 25.0);
    CHECK_THROWS_AS(sketch.setConstraintValue(length, -3.0), std::invalid_argument);
    CHECK(sketch.findConstraint(length)->value == 25.0);
    CHECK_THROWS_AS(sketch.setConstraintValue(level, 1.0), std::invalid_argument);
    CHECK_THROWS_AS(sketch.setConstraintValue(999, 1.0), std::invalid_argument);

    sketch.removeConstraint(level);
    CHECK(sketch.findConstraint(level) == nullptr);
    CHECK_THROWS_AS(sketch.removeConstraint(level), std::invalid_argument);
}

TEST_CASE("Fix records where the point is", "[sketch]") {
    Sketch sketch;
    const EntityId point = sketch.addPoint({3.5, -2.0});

    const ConstraintId fixed = sketch.addConstraint(ConstraintKind::Fix, point);

    CHECK(sketch.findConstraint(fixed)->value == 3.5);
    CHECK(sketch.findConstraint(fixed)->value2 == -2.0);
}

TEST_CASE("Dimensions can be measured before they are added", "[sketch]") {
    Sketch sketch;
    const EntityId line = sketch.addLine({0.0, 0.0}, {3.0, 4.0});
    const EntityId up = sketch.addLine({0.0, 0.0}, {0.0, 2.0});
    const EntityId circle = sketch.addCircle({0.0, 0.0}, 6.0);
    const EntityId point = sketch.addPoint({3.0, 0.0});
    const Entity* l = sketch.find(line);

    CHECK_THAT(sketch.measure(ConstraintKind::Distance, l->start, l->end),
               WithinAbs(5.0, lengthTolerance));
    CHECK_THAT(sketch.measure(ConstraintKind::DistanceX, l->start, l->end),
               WithinAbs(3.0, lengthTolerance));
    CHECK_THAT(sketch.measure(ConstraintKind::DistanceY, l->start, l->end),
               WithinAbs(4.0, lengthTolerance));
    CHECK_THAT(sketch.measure(ConstraintKind::Radius, circle), WithinAbs(6.0, lengthTolerance));
    CHECK_THAT(sketch.measure(ConstraintKind::Diameter, circle), WithinAbs(12.0, lengthTolerance));
    // From (3, 0) to the line through the origin and (3, 4): 3 * 4 / 5.
    CHECK_THAT(sketch.measure(ConstraintKind::PointLineDistance, point, line),
               WithinAbs(2.4, lengthTolerance));
    // From the sloping line counter-clockwise to the vertical one.
    CHECK_THAT(sketch.measure(ConstraintKind::Angle, line, up),
               WithinAbs(std::atan2(3.0, 4.0), 1e-12));
    CHECK_THROWS_AS(sketch.measure(ConstraintKind::Parallel, line, up), std::invalid_argument);
}

TEST_CASE("Constraint kinds have names that read back", "[sketch]") {
    for (const ConstraintKind kind :
         {ConstraintKind::Coincident,    ConstraintKind::PointOnLine,
          ConstraintKind::PointOnCircle, ConstraintKind::Horizontal,
          ConstraintKind::Vertical,      ConstraintKind::Parallel,
          ConstraintKind::Perpendicular, ConstraintKind::Tangent,
          ConstraintKind::Equal,         ConstraintKind::Concentric,
          ConstraintKind::Midpoint,      ConstraintKind::Symmetric,
          ConstraintKind::Collinear,     ConstraintKind::Fix,
          ConstraintKind::Distance,      ConstraintKind::DistanceX,
          ConstraintKind::DistanceY,     ConstraintKind::PointLineDistance,
          ConstraintKind::Radius,        ConstraintKind::Diameter,
          ConstraintKind::Angle}) {
        CAPTURE(toString(kind));
        CHECK(constraintKindFromString(toString(kind)) == kind);
    }
    CHECK_FALSE(constraintKindFromString("Wobbly").has_value());
    CHECK(isDimension(ConstraintKind::Angle));
    CHECK_FALSE(isDimension(ConstraintKind::Tangent));
}

TEST_CASE("A sketch rebuilt from stored parts is the same sketch, and bad parts are refused",
          "[sketch]") {
    Sketch original;
    const EntityId line = original.addLine({0.0, 0.0}, {10.0, 0.0});
    original.addCircle({5.0, 5.0}, 2.0);
    original.addConstraint(ConstraintKind::Horizontal, line);

    const Sketch copy(original.entities(), original.constraints(), original.nextEntityId(),
                      original.nextConstraintId());
    CHECK(copy.origin() == original.origin());
    CHECK(copy.entities().size() == original.entities().size());
    CHECK(copy.constraints().size() == 1);
    CHECK(copy.find(line)->kind == EntityKind::Line);

    SECTION("a line whose point is missing") {
        std::vector<Entity> entities = original.entities();
        std::erase_if(entities,
                      [&](const Entity& e) { return e.id == original.find(line)->start; });
        CHECK_THROWS_AS(Sketch(entities, {}, original.nextEntityId(), 1), std::invalid_argument);
    }
    SECTION("no origin") {
        std::vector<Entity> entities = original.entities();
        std::erase_if(entities, [&](const Entity& e) { return e.id == original.origin(); });
        CHECK_THROWS_AS(Sketch(entities, {}, original.nextEntityId(), 1), std::invalid_argument);
    }
    SECTION("a constraint on the wrong kind of entity") {
        Constraint wrong;
        wrong.id = 1;
        wrong.kind = ConstraintKind::Radius;
        wrong.first = line;
        wrong.value = 2.0;
        CHECK_THROWS_AS(Sketch(original.entities(), {wrong}, original.nextEntityId(), 2),
                        std::invalid_argument);
    }
    SECTION("an id beyond those handed out") {
        CHECK_THROWS_AS(Sketch(original.entities(), {}, 2, 1), std::invalid_argument);
    }
}

TEST_CASE("Reference geometry stays where it is put", "[sketch]") {
    Sketch sketch;
    const EntityId wall = sketch.addLine({30.0, -20.0}, {30.0, 20.0});
    const EntityId hole = sketch.addCircle({10.0, 10.0}, 4.0);
    const EntityId mark = sketch.addPoint({-5.0, 5.0});
    for (const EntityId id : {wall, hole, mark}) {
        sketch.setFixed(id, true);
        CHECK(sketch.find(id)->fixed);
    }
    // Nothing of it counts as free to move, and dragging it does nothing.
    SolveResult state = sketch.solve();
    CHECK(state.degreesOfFreedom == 0);
    CHECK(state.underconstrained.empty());
    sketch.dragBy(wall, 5.0, 5.0);
    sketch.dragRadius(hole, {30.0, 10.0});
    CHECK(sketch.position(sketch.find(wall)->start).x == 30.0);
    CHECK(sketch.find(hole)->radius == 4.0);

    // What is constrained to it moves to it; it does not move to meet that.
    const EntityId line = sketch.addLine({0.0, 1.0}, {20.0, 3.0});
    sketch.addConstraint(ConstraintKind::PointOnLine, sketch.find(line)->end, wall);
    sketch.addConstraint(ConstraintKind::Coincident, sketch.find(line)->start, mark);
    state = sketch.solve();
    CHECK(state.status == SolveStatus::Solved);
    CHECK(std::abs(sketch.position(sketch.find(line)->end).x - 30.0) < 1e-9);
    CHECK(sketch.position(sketch.find(line)->start).x == -5.0);
    CHECK(sketch.position(sketch.find(wall)->start).x == 30.0);
    CHECK(sketch.position(sketch.find(wall)->end).y == 20.0);
    // One freedom is left: the line's end can slide along the wall.
    CHECK(state.degreesOfFreedom == 1);
    CHECK(state.underconstrained == std::vector<EntityId>{line});

    // It can be made ordinary again, and then it is as free as anything.
    sketch.setFixed(hole, false);
    CHECK(sketch.solve().degreesOfFreedom == 4);
    CHECK_THROWS_AS(sketch.setFixed(sketch.origin(), false), std::invalid_argument);
    CHECK_THROWS_AS(sketch.setFixed(sketch.find(line)->start, true), std::invalid_argument);
    CHECK_THROWS_AS(sketch.setFixed(9999, true), std::invalid_argument);
}
