// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/sketch/Edit.h>
#include <lcad/sketch/Sketch.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <stdexcept>
#include <vector>

using namespace lcad::sketch;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

const double pi = 3.14159265358979323846;

double apart(Point2 a, Point2 b) {
    return std::hypot(b.x - a.x, b.y - a.y);
}

} // namespace

TEST_CASE("An ellipse has a center, the end of its major axis, and a minor radius",
          "[sketch][curves]") {
    Sketch s;
    const EntityId e = s.addEllipse({10.0, 5.0}, {40.0, 5.0}, 10.0);
    const Entity* ellipse = s.find(e);
    REQUIRE(ellipse != nullptr);
    CHECK(ellipse->kind == EntityKind::Ellipse);
    CHECK(s.find(ellipse->center)->owner == e);
    CHECK(s.find(ellipse->start)->owner == e);
    CHECK(s.radiusOf(e) == 10.0);
    // Round from the end of the major axis, counter-clockwise.
    CHECK_THAT(apart(s.pointAt(e, 0.0), {40.0, 5.0}), WithinAbs(0.0, 1e-12));
    CHECK_THAT(apart(s.pointAt(e, 0.25), {10.0, 15.0}), WithinAbs(0.0, 1e-12));
    CHECK_THAT(apart(s.pointAt(e, 0.5), {-20.0, 5.0}), WithinAbs(0.0, 1e-12));
    CHECK(s.samples(e, 8).size() == 9);
    // Every sample lies on it.
    for (const Point2 p : s.samples(e, 64)) {
        const double u = (p.x - 10.0) / 30.0;
        const double v = (p.y - 5.0) / 10.0;
        CHECK_THAT(u * u + v * v, WithinAbs(1.0, 1e-12));
    }
    // Sizes that describe no ellipse, or with the minor radius the larger, are refused.
    CHECK_THROWS_AS(s.addEllipse({0.0, 0.0}, {10.0, 0.0}, 0.0), std::invalid_argument);
    CHECK_THROWS_AS(s.addEllipse({0.0, 0.0}, {10.0, 0.0}, 12.0), std::invalid_argument);

    // Its center on the origin, its major axis level and 30 long, its minor radius 12: fully
    // constrained.
    REQUIRE(tryConstraint(s, ConstraintKind::Coincident, ellipse->center, s.origin()).ok());
    REQUIRE(tryConstraint(s, ConstraintKind::Horizontal, s.find(e)->center, s.find(e)->start).ok());
    REQUIRE(tryConstraint(s, ConstraintKind::Distance, s.find(e)->center, s.find(e)->start, 0, 30.0)
                .ok());
    // The dimension tool gives an ellipse its minor radius.
    const auto choice = dimensionFor(s, {e});
    REQUIRE(choice.has_value());
    CHECK(choice->kind == ConstraintKind::Radius);
    CHECK(choice->value == 10.0);
    const Outcome minor = tryConstraint(s, ConstraintKind::Radius, e, 0, 0, 12.0);
    REQUIRE(minor.ok());
    CHECK(minor.result.isFullyConstrained());
    CHECK(s.radiusOf(e) == 12.0);
    CHECK_THAT(apart(s.position(s.find(e)->start), {30.0, 0.0}), WithinAbs(0.0, 1e-9));
    // The same again, or a contradiction, is refused.
    CHECK(tryConstraint(s, ConstraintKind::Radius, e, 0, 0, 12.0).refusal == Refusal::Redundant);
    CHECK(tryConstraint(s, ConstraintKind::Diameter, e, 0, 0, 30.0).refusal ==
          Refusal::Conflicting);

    // Its nearest point to something outside is on it.
    const Point2 nearest = nearestOn(s, e, {0.0, 40.0});
    CHECK_THAT(nearest.x, WithinAbs(0.0, 0.05));
    CHECK_THAT(nearest.y, WithinAbs(12.0, 0.05));
    CHECK_THAT(distanceTo(s, e, {0.0, 40.0}), WithinAbs(28.0, 0.05));
}

TEST_CASE("An ellipse alone is a closed outline, and can hold a hole", "[sketch][curves]") {
    Sketch s;
    const EntityId e = s.addEllipse({0.0, 0.0}, {30.0, 0.0}, 12.0);
    const EntityId hole = s.addCircle({5.0, 0.0}, 3.0);
    const std::vector<Profile> outlines = s.profiles();
    REQUIRE(outlines.size() == 2);
    const std::vector<Region> regions = s.regions();
    REQUIRE(regions.size() == 2);
    const Region& outer = regions[0].outer == std::vector<EntityId>{e} ? regions[0] : regions[1];
    CHECK(outer.holes == std::vector<std::vector<EntityId>>{{hole}});
    CHECK_THAT(outer.area, WithinRel(pi * 30.0 * 12.0 - pi * 9.0, 1e-9));
    // Dragged as a whole, it moves; its minor radius follows the pointer.
    s.dragBy(e, 5.0, 0.0);
    CHECK_THAT(s.position(s.find(e)->center).x, WithinAbs(5.0, 1e-9));
    s.dragRadius(e, {5.0, 20.0});
    CHECK_THAT(s.radiusOf(e), WithinAbs(20.0, 1e-9));
}

TEST_CASE("A spline runs from its first control point to its last", "[sketch][curves]") {
    Sketch s;
    const std::vector<Point2> controls{{0.0, 0.0}, {10.0, 20.0}, {30.0, 20.0}, {40.0, 0.0}};
    const EntityId curve = s.addSpline(controls);
    const Entity* spline = s.find(curve);
    REQUIRE(spline != nullptr);
    CHECK(spline->points.size() == 4);
    CHECK(spline->start == spline->points.front());
    CHECK(spline->end == spline->points.back());
    CHECK(splineDegree(4) == 3);
    CHECK(splineDegree(3) == 2);
    CHECK(splineDegree(10) == 3);
    CHECK_THAT(apart(s.pointAt(curve, 0.0), {0.0, 0.0}), WithinAbs(0.0, 1e-12));
    CHECK_THAT(apart(s.pointAt(curve, 1.0), {40.0, 0.0}), WithinAbs(0.0, 1e-12));
    // A cubic Bézier, as four control points are: its middle is at (20, 15).
    CHECK_THAT(apart(s.pointAt(curve, 0.5), {20.0, 15.0}), WithinAbs(0.0, 1e-12));
    // With more control points it passes near them, not through them, and stays smooth.
    const EntityId longer =
        s.addSpline({{0.0, 50.0}, {10.0, 60.0}, {20.0, 50.0}, {30.0, 60.0}, {40.0, 50.0}});
    CHECK_THAT(apart(s.pointAt(longer, 1.0), {40.0, 50.0}), WithinAbs(0.0, 1e-12));
    CHECK(s.pointAt(longer, 0.5).y > 50.0);
    CHECK(s.pointAt(longer, 0.5).y < 60.0);

    // Too few control points, or two in one place, are refused.
    CHECK_THROWS_AS(s.addSpline({{0.0, 0.0}, {1.0, 1.0}}), std::invalid_argument);
    CHECK_THROWS_AS(s.addSpline({{0.0, 0.0}, {0.0, 0.0}, {1.0, 1.0}}), std::invalid_argument);

    // Its control points are points like any: an end joined to a line's end closes an outline.
    const EntityId base = s.addLine({40.0, 0.0}, {0.0, 0.0});
    REQUIRE(
        tryConstraint(s, ConstraintKind::Coincident, s.find(curve)->end, s.find(base)->start).ok());
    REQUIRE(
        tryConstraint(s, ConstraintKind::Coincident, s.find(base)->end, s.find(curve)->start).ok());
    const std::vector<Profile> outlines = s.profiles();
    REQUIRE(outlines.size() == 1);
    // The area under this cubic, worked out by hand from x(t) and y(t): 420. Chords every
    // 1/120 of the way come within a few thousandths.
    CHECK_THAT(outlines.front().area, WithinRel(420.0, 1e-4));
    // Dragged up by 10, a control point raises the curve by 3·(1-t)²·t·10 at t: 4.22 at 1/4.
    const double before = s.pointAt(curve, 0.25).y;
    s.dragBy(s.find(curve)->points[1], 0.0, 10.0);
    CHECK_THAT(s.pointAt(curve, 0.25).y - before, WithinAbs(3.0 * 0.5625 * 0.25 * 10.0, 1e-9));
    CHECK_THAT(apart(s.position(s.find(curve)->start), s.position(s.find(base)->end)),
               WithinAbs(0.0, 1e-9));
    // Removing it removes its control points.
    const std::size_t count = s.entities().size();
    s.remove(curve);
    CHECK(s.entities().size() == count - 5);
}

TEST_CASE("Ellipses and splines are mirrored; trim, offset, and crossings leave them alone",
          "[sketch][curves]") {
    Sketch s;
    const EntityId axis = s.addLine({0.0, -50.0}, {0.0, 50.0});
    const EntityId e = s.addEllipse({20.0, 0.0}, {35.0, 5.0}, 6.0);
    const EntityId curve = s.addSpline({{10.0, 20.0}, {20.0, 30.0}, {30.0, 20.0}});
    const std::vector<EntityId> images = mirror(s, {e, curve}, axis);
    REQUIRE(images.size() == 2);
    const Entity* image = s.find(images[0]);
    CHECK(image->kind == EntityKind::Ellipse);
    CHECK_THAT(s.position(image->center).x, WithinAbs(-20.0, 1e-9));
    CHECK_THAT(s.position(image->start).x, WithinAbs(-35.0, 1e-9));
    CHECK_THAT(s.radiusOf(images[0]), WithinAbs(6.0, 1e-9));
    CHECK_THAT(s.position(s.find(images[1])->points[1]).x, WithinAbs(-20.0, 1e-9));
    // Held in its image: a change to the original's minor radius follows.
    REQUIRE(tryConstraint(s, ConstraintKind::Radius, e, 0, 0, 8.0).ok());
    CHECK_THAT(s.radiusOf(images[0]), WithinAbs(8.0, 1e-9));

    const EntityId line = s.addLine({20.0, -20.0}, {20.0, 20.0});
    CHECK(intersections(s, e, line).empty());
    CHECK_FALSE(trim(s, e, {35.0, 5.0}));
    CHECK_FALSE(trim(s, curve, {20.0, 25.0}));
    CHECK_THROWS_AS(offset(s, {e}, 2.0, {0.0, 0.0}), std::invalid_argument);
}
