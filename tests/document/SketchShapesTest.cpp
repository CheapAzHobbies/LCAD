// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/document/SketchShapes.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Plane.h>

#include <Tolerances.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace lcad::document;
using Catch::Matchers::WithinAbs;
using lcad::test::lengthTolerance;

namespace {
const double pi = 3.14159265358979323846;
}

TEST_CASE("A sketch's geometry becomes edges on its plane", "[document][sketch]") {
    Document document;
    // On the XZ plane: u runs along X, v along Z, and the sketch lies at y = 0.
    const ObjectId id = document.addSketch("Sketch 1", lcad::kernel::planeXZ());
    lcad::sketch::Sketch geometry;
    geometry.addLine({0.0, 0.0}, {30.0, 40.0});
    geometry.addCircle({10.0, 10.0}, 5.0);
    // A quarter circle of radius 8.
    geometry.addArc({50.0, 0.0}, {58.0, 0.0}, {50.0, 8.0});
    geometry.addPoint({3.0, 4.0});
    const auto guide = geometry.addLine({0.0, 20.0}, {100.0, 20.0});
    geometry.setConstruction(guide, true);
    document.setSketchGeometry(id, geometry, "Draw");
    const SketchItem& sketch = *document.findSketch(id);

    const lcad::kernel::Shape ordinary = sketchShape(sketch, SketchPart::Ordinary);
    REQUIRE_FALSE(ordinary.isNull());
    const lcad::kernel::TopologyCount count = lcad::kernel::countTopology(ordinary);
    CHECK(count.edges == 3);
    CHECK(count.solids == 0);
    // The total length says each curve is the one that was drawn: the line, the whole circle,
    // and a quarter of the other, not its other three quarters.
    CHECK_THAT(lcad::kernel::length(ordinary),
               WithinAbs(50.0 + 2.0 * pi * 5.0 + pi * 8.0 / 2.0, 1e-6));
    const auto box = lcad::kernel::boundingBox(ordinary);
    REQUIRE(box.has_value());
    // In the XZ plane: flat in Y, and v became Z.
    CHECK_THAT(box->min.y, WithinAbs(0.0, 1e-6));
    CHECK_THAT(box->max.y, WithinAbs(0.0, 1e-6));
    CHECK_THAT(box->max.x, WithinAbs(58.0, 1e-5));
    CHECK_THAT(box->max.z, WithinAbs(40.0, 1e-5));

    const lcad::kernel::Shape construction = sketchShape(sketch, SketchPart::Construction);
    REQUIRE_FALSE(construction.isNull());
    CHECK(lcad::kernel::countTopology(construction).edges == 1);
    CHECK_THAT(lcad::kernel::length(construction), WithinAbs(100.0, 1e-6));

    const auto bounds = sketchBounds(sketch);
    REQUIRE(bounds.has_value());
    CHECK_THAT(bounds->min.x, WithinAbs(0.0, lengthTolerance));
    CHECK_THAT(bounds->max.x, WithinAbs(100.0, lengthTolerance));
    CHECK_THAT(bounds->max.z, WithinAbs(40.0, lengthTolerance));
    CHECK_THAT(bounds->min.y, WithinAbs(0.0, lengthTolerance));
}

TEST_CASE("An empty sketch has no shape and no bounds", "[document][sketch]") {
    Document document;
    const ObjectId id = document.addSketch("Sketch 1", lcad::kernel::planeXY());
    const SketchItem& sketch = *document.findSketch(id);
    CHECK(sketchShape(sketch, SketchPart::Ordinary).isNull());
    CHECK(sketchShape(sketch, SketchPart::Construction).isNull());
    CHECK_FALSE(sketchBounds(sketch).has_value());
}
