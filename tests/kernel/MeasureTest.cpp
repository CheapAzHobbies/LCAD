// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Measure.h>
#include <lcad/kernel/occt/ShapeAccess.h>

#include <Tolerances.h>

#include <BRepBuilderAPI_MakeFace.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pln.hxx>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace lcad::kernel;
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
