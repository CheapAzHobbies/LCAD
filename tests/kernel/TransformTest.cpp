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
#include <optional>
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

namespace {

constexpr double pi = 3.14159265358979323846;

void requirePoint(const Point& point, double x, double y, double z) {
    CHECK_THAT(point.x, WithinAbs(x, lengthTolerance));
    CHECK_THAT(point.y, WithinAbs(y, lengthTolerance));
    CHECK_THAT(point.z, WithinAbs(z, lengthTolerance));
}

void requireVector(const Vector& vector, double x, double y, double z) {
    CHECK_THAT(vector.x, WithinAbs(x, lengthTolerance));
    CHECK_THAT(vector.y, WithinAbs(y, lengthTolerance));
    CHECK_THAT(vector.z, WithinAbs(z, lengthTolerance));
}

} // namespace

TEST_CASE("Placements turn and move points, and undo each other", "[kernel][transform]") {
    // A quarter turn about Z, then a move along X.
    const Placement turn = rotation(Axis{{0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}}, pi / 2.0);
    const Placement move = translation({10.0, 0.0, 0.0});
    REQUIRE(isRigid(turn));

    requirePoint(apply(turn, Point{1.0, 0.0, 0.0}), 0.0, 1.0, 0.0);
    // Inner first: turned, then moved.
    requirePoint(apply(move * turn, Point{1.0, 0.0, 0.0}), 10.0, 1.0, 0.0);
    requirePoint(apply(turn * move, Point{1.0, 0.0, 0.0}), 0.0, 11.0, 0.0);
    // A direction is turned and not moved.
    requireVector(apply(move * turn, Vector{1.0, 0.0, 0.0}), 0.0, 1.0, 0.0);

    const Placement both = move * turn;
    CHECK(samePlacement(inverse(both) * both, Placement{}));
    CHECK(samePlacement(both * inverse(both), Placement{}));
    requirePoint(apply(inverse(both), Point{10.0, 1.0, 0.0}), 1.0, 0.0, 0.0);

    SECTION("a turn about a line off the origin keeps the line where it is") {
        const Placement about = rotation(Axis{{5.0, 0.0, 0.0}, {0.0, 0.0, 2.0}}, pi);
        requirePoint(apply(about, Point{5.0, 0.0, 7.0}), 5.0, 0.0, 7.0);
        requirePoint(apply(about, Point{6.0, 0.0, 0.0}), 4.0, 0.0, 0.0);
    }
    SECTION("a positive turn is counter-clockwise seen from the tip of the axis") {
        const Placement aboutX = rotation(Axis{{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}}, pi / 2.0);
        requireVector(apply(aboutX, Vector{0.0, 1.0, 0.0}), 0.0, 0.0, 1.0);
    }
    SECTION("planes and lines go with it") {
        const Plane plane = apply(both, planeXY());
        requirePoint(plane.origin, 10.0, 0.0, 0.0);
        requireVector(plane.normal, 0.0, 0.0, 1.0);
        requireVector(plane.xDirection, 0.0, 1.0, 0.0);
        const Axis axis = apply(both, Axis{{1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}});
        requirePoint(axis.origin, 10.0, 1.0, 0.0);
        requireVector(axis.direction, 0.0, 1.0, 0.0);
    }
}

TEST_CASE("Frames are built square and right-handed", "[kernel][transform]") {
    const Placement fromPlane = frameOf(planeXZ());
    CHECK(isRigid(fromPlane));
    requireVector(fromPlane.zAxis, 0.0, -1.0, 0.0);
    requireVector(fromPlane.xAxis, 1.0, 0.0, 0.0);
    requireVector(fromPlane.yAxis, 0.0, 0.0, 1.0);

    // X follows the model's X as far as Z allows, and the model's Y where Z is the model's X.
    const Placement tilted = frameAt({1.0, 2.0, 3.0}, {0.0, 1.0, 1.0});
    CHECK(isRigid(tilted));
    requireVector(tilted.xAxis, 1.0, 0.0, 0.0);
    const Placement alongX = frameAt({0.0, 0.0, 0.0}, {-3.0, 0.0, 0.0});
    CHECK(isRigid(alongX));
    requireVector(alongX.zAxis, -1.0, 0.0, 0.0);
    requireVector(alongX.xAxis, 0.0, 1.0, 0.0);

    CHECK_THROWS_AS(frameAt({0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}), std::invalid_argument);
    CHECK_FALSE(
        isRigid(Placement{{0.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}}));
    // A mirror image is not a placement.
    CHECK_FALSE(
        isRigid(Placement{{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, -1.0}}));

    SECTION("drift is taken out again") {
        const Placement drifted{
            {0.0, 0.0, 0.0}, {1.0, 1e-4, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0001}};
        CHECK(isRigid(normalized(drifted)));
    }
}

TEST_CASE("placed turns and moves a shape", "[kernel][transform]") {
    const Shape box = makeBox(10.0, 20.0, 30.0);
    const Placement placement =
        translation({100.0, 0.0, 0.0}) * rotation(Axis{{0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}}, pi / 2.0);

    const Shape moved = placed(box, placement);

    REQUIRE(isValid(moved));
    CHECK_THAT(volume(moved), WithinRel(6000.0, exactPropertyTolerance));
    const auto bounds = boundingBox(moved);
    REQUIRE(bounds.has_value());
    // Turned a quarter about Z, the box reaches from -20 to 0 along X, then moved by 100.
    CHECK_THAT(bounds->min.x, WithinAbs(80.0, lengthTolerance));
    CHECK_THAT(bounds->max.x, WithinAbs(100.0, lengthTolerance));
    CHECK_THAT(bounds->min.y, WithinAbs(0.0, lengthTolerance));
    CHECK_THAT(bounds->max.y, WithinAbs(10.0, lengthTolerance));
    CHECK(placed(Shape(), placement).isNull());
    CHECK_THROWS_AS(
        placed(box, Placement{{0.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}}),
        std::invalid_argument);
}

TEST_CASE("Parts of a shape give frames to line components up by", "[kernel][transform]") {
    const Shape box = makeBox(10.0, 20.0, 30.0);

    SECTION("a flat face: its center, its normal out of the solid") {
        // The top of the box is the face whose normal points up.
        std::optional<Placement> top;
        for (int i = 0; i < subShapeCount(box, SubShapeKind::Face); ++i) {
            const auto frame = frameOn(subShape(box, SubShapeKind::Face, i));
            REQUIRE(frame.has_value());
            CHECK(isRigid(*frame));
            if (frame->zAxis.z > 0.5) {
                top = frame;
            }
        }
        REQUIRE(top.has_value());
        requirePoint(top->origin, 5.0, 10.0, 30.0);
    }
    SECTION("a cylinder's side: the middle of its axis, or either end") {
        const Shape cylinder = translated(makeCylinder(5.0, 40.0), 0.0, 0.0, 10.0);
        std::optional<Shape> side;
        for (int i = 0; i < subShapeCount(cylinder, SubShapeKind::Face); ++i) {
            const Shape face = subShape(cylinder, SubShapeKind::Face, i);
            if (radiusOf(face).has_value()) {
                side = face;
            }
        }
        REQUIRE(side.has_value());
        const auto middle = frameOn(*side);
        REQUIRE(middle.has_value());
        requirePoint(middle->origin, 0.0, 0.0, 30.0);
        CHECK_THAT(std::abs(middle->zAxis.z), WithinAbs(1.0, lengthTolerance));
        const auto start = frameOn(*side, FrameSnap::Start);
        const auto end = frameOn(*side, FrameSnap::End);
        REQUIRE(start.has_value());
        REQUIRE(end.has_value());
        CHECK_THAT(std::min(start->origin.z, end->origin.z), WithinAbs(10.0, lengthTolerance));
        CHECK_THAT(std::max(start->origin.z, end->origin.z), WithinAbs(50.0, lengthTolerance));
    }
    SECTION("a circular edge: its center and its axis") {
        const Shape cylinder = translated(makeCylinder(5.0, 40.0), 3.0, 4.0, 0.0);
        bool found = false;
        for (int i = 0; i < subShapeCount(cylinder, SubShapeKind::Edge); ++i) {
            const Shape edge = subShape(cylinder, SubShapeKind::Edge, i);
            if (!radiusOf(edge).has_value()) {
                continue;
            }
            const auto frame = frameOn(edge);
            REQUIRE(frame.has_value());
            CHECK_THAT(frame->origin.x, WithinAbs(3.0, lengthTolerance));
            CHECK_THAT(frame->origin.y, WithinAbs(4.0, lengthTolerance));
            CHECK_THAT(std::abs(frame->zAxis.z), WithinAbs(1.0, lengthTolerance));
            found = true;
        }
        CHECK(found);
    }
    SECTION("a straight edge: its middle, start, or end, along it") {
        const Shape edge = makeSegment({0.0, 0.0, 0.0}, {10.0, 0.0, 0.0});
        requirePoint(frameOn(edge)->origin, 5.0, 0.0, 0.0);
        requirePoint(frameOn(edge, FrameSnap::Start)->origin, 0.0, 0.0, 0.0);
        requirePoint(frameOn(edge, FrameSnap::End)->origin, 10.0, 0.0, 0.0);
        requireVector(frameOn(edge)->zAxis, 1.0, 0.0, 0.0);
    }
    SECTION("a vertex: the point, upright") {
        const auto frame = frameOn(makeVertex({1.0, 2.0, 3.0}));
        REQUIRE(frame.has_value());
        requirePoint(frame->origin, 1.0, 2.0, 3.0);
        requireVector(frame->zAxis, 0.0, 0.0, 1.0);
    }
    SECTION("a placed shape gives its frames where it is placed") {
        const Shape moved = placed(box, translation({0.0, 0.0, 100.0}));
        bool found = false;
        for (int i = 0; i < subShapeCount(moved, SubShapeKind::Face); ++i) {
            const auto frame = frameOn(subShape(moved, SubShapeKind::Face, i));
            if (frame.has_value() && frame->zAxis.z > 0.5) {
                requirePoint(frame->origin, 5.0, 10.0, 130.0);
                found = true;
            }
        }
        CHECK(found);
    }
    CHECK_FALSE(frameOn(Shape()).has_value());
}
