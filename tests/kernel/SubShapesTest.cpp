// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/kernel/occt/ShapeAccess.h>

#include <Tolerances.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <stdexcept>
#include <vector>

using namespace lcad::kernel;
using Catch::Matchers::WithinRel;
using lcad::test::exactPropertyTolerance;

TEST_CASE("The sub-shapes of a box can be counted and fetched", "[kernel][subshapes]") {
    const Shape box = makeBox(10.0, 20.0, 30.0);

    CHECK(subShapeCount(box, SubShapeKind::Face) == 6);
    CHECK(subShapeCount(box, SubShapeKind::Edge) == 12);
    CHECK(subShapeCount(box, SubShapeKind::Vertex) == 8);

    SECTION("the faces are the six sides, each of the right area") {
        std::vector<double> areas;
        for (int i = 0; i < 6; ++i) {
            const Shape face = subShape(box, SubShapeKind::Face, i);
            CHECK(countTopology(face).faces == 1);
            areas.push_back(surfaceArea(face));
        }
        std::sort(areas.begin(), areas.end());
        CHECK_THAT(areas[0], WithinRel(200.0, exactPropertyTolerance));
        CHECK_THAT(areas[1], WithinRel(200.0, exactPropertyTolerance));
        CHECK_THAT(areas[2], WithinRel(300.0, exactPropertyTolerance));
        CHECK_THAT(areas[3], WithinRel(300.0, exactPropertyTolerance));
        CHECK_THAT(areas[4], WithinRel(600.0, exactPropertyTolerance));
        CHECK_THAT(areas[5], WithinRel(600.0, exactPropertyTolerance));
    }

    SECTION("the edges add up to the box's twelve edges") {
        double total = 0.0;
        for (int i = 0; i < 12; ++i) {
            total += length(subShape(box, SubShapeKind::Edge, i));
        }
        CHECK_THAT(total, WithinRel(4.0 * (10.0 + 20.0 + 30.0), exactPropertyTolerance));
        // Measuring the whole box counts each shared edge once.
        CHECK_THAT(length(box), WithinRel(240.0, exactPropertyTolerance));
    }

    SECTION("a sub-shape finds its own number again") {
        for (const SubShapeKind kind :
             {SubShapeKind::Face, SubShapeKind::Edge, SubShapeKind::Vertex}) {
            for (int i = 0; i < subShapeCount(box, kind); ++i) {
                CHECK(occt::subShapeIndex(box, occt::unwrap(subShape(box, kind, i))) == i);
            }
        }
    }

    SECTION("an index outside the range is refused") {
        CHECK_THROWS_AS(subShape(box, SubShapeKind::Face, 6), std::out_of_range);
        CHECK_THROWS_AS(subShape(box, SubShapeKind::Face, -1), std::out_of_range);
    }
}

TEST_CASE("Sub-shape lookups cope with shapes that are null or unrelated", "[kernel][subshapes]") {
    const Shape box = makeBox(1.0, 2.0, 3.0);
    const Shape other = makeBox(1.0, 2.0, 3.0);

    CHECK(subShapeCount(Shape(), SubShapeKind::Face) == 0);
    CHECK_THROWS_AS(subShape(Shape(), SubShapeKind::Face, 0), std::out_of_range);
    CHECK(length(Shape()) == 0.0);
    // A face of another box is not a face of this one, however alike they are.
    CHECK(occt::subShapeIndex(box, occt::unwrap(subShape(other, SubShapeKind::Face, 0))) == -1);
    // A solid is not a pickable sub-shape.
    CHECK(occt::subShapeIndex(box, occt::unwrap(box)) == -1);
    CHECK(occt::subShapeIndex(box, TopoDS_Shape()) == -1);
}
