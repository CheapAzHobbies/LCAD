// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Error.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/Projection.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/kernel/Transform.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

using namespace lcad::kernel;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double pi = 3.14159265358979323846;

// Looking along +Y, at the front: X to the right, Z up.
const ViewFrame front{{1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}};
// Looking down, from above: X to the right, Y up.
const ViewFrame top{{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}};

double lengthOf(const ProjectedCurve& curve) {
    double total = 0.0;
    for (std::size_t i = 1; i < curve.points.size(); ++i) {
        total += std::hypot(curve.points[i].x - curve.points[i - 1].x,
                            curve.points[i].y - curve.points[i - 1].y);
    }
    return total;
}

double totalLength(const std::vector<ProjectedCurve>& curves) {
    return std::accumulate(curves.begin(), curves.end(), 0.0,
                           [](double sum, const ProjectedCurve& c) { return sum + lengthOf(c); });
}

// A plate of 60 by 40 by 10, with a hole of radius 5 through it at (30, 20).
NamedShape plateWithHole() {
    const NamedShape plate = nameFaces(makeBox(60.0, 40.0, 10.0), "p");
    const NamedShape hole = nameFaces(translated(makeCylinder(5.0, 12.0), 30.0, 20.0, -1.0), "h");
    return combine(plate, hole, Combination::Cut);
}

} // namespace

TEST_CASE("A view shows the outline of a box and nothing behind it", "[kernel][projection]") {
    const Projection view = project(makeBox(60.0, 40.0, 10.0), front);
    // The front of a box is a rectangle; what is behind it lies exactly behind its edges.
    CHECK_THAT(totalLength(view.visible), WithinAbs(2.0 * (60.0 + 10.0), 1e-6));
    double lowest = 1e9;
    double highest = -1e9;
    for (const ProjectedCurve& curve : view.visible) {
        for (const Point2D& p : curve.points) {
            lowest = std::min(lowest, p.y);
            highest = std::max(highest, p.y);
        }
    }
    CHECK_THAT(lowest, WithinAbs(0.0, 1e-6));
    CHECK_THAT(highest, WithinAbs(10.0, 1e-6));
}

TEST_CASE("A hole shows as a circle from above and hidden lines from the side",
          "[kernel][projection]") {
    const Shape plate = plateWithHole().shape;

    const Projection above = project(plate, top);
    const auto circle = std::find_if(above.visible.begin(), above.visible.end(),
                                     [](const ProjectedCurve& c) { return c.isArc; });
    REQUIRE(circle != above.visible.end());
    CHECK_THAT(circle->radius, WithinAbs(5.0, 1e-6));
    CHECK_THAT(circle->center.x, WithinAbs(30.0, 1e-6));
    CHECK_THAT(circle->center.y, WithinAbs(20.0, 1e-6));
    double circumference = 0.0;
    for (const ProjectedCurve& curve : above.visible) {
        circumference += curve.isArc ? lengthOf(curve) : 0.0;
    }
    // The top rim. The bottom rim is exactly behind it.
    CHECK_THAT(circumference, WithinAbs(2.0 * pi * 5.0, 0.05));

    // From the front the hole's sides are behind the plate's front: two lines from top to
    // bottom, ten apart.
    const Projection side = project(plate, front);
    CHECK_THAT(totalLength(side.hidden), WithinAbs(20.0, 1e-6));
    double left = 1e9;
    double right = -1e9;
    for (const ProjectedCurve& curve : side.hidden) {
        for (const Point2D& p : curve.points) {
            left = std::min(left, p.x);
            right = std::max(right, p.x);
        }
    }
    CHECK_THAT(left, WithinAbs(25.0, 1e-6));
    CHECK_THAT(right, WithinAbs(35.0, 1e-6));
}

TEST_CASE("An arc keeps its exact form, however the view turns it", "[kernel][projection]") {
    // A quarter of a cylinder: one quarter circle seen from above.
    const NamedShape round = nameFaces(makeCylinder(10.0, 5.0), "c");
    const NamedShape box = nameFaces(translated(makeBox(30.0, 30.0, 9.0), 0.0, 0.0, -2.0), "b");
    const Shape quarter = combine(round, box, Combination::Intersect).shape;
    for (const ViewFrame& frame : {top, ViewFrame{{-1.0, 0.0, 0.0}, {0.0, -1.0, 0.0}}}) {
        const Projection view = project(quarter, frame);
        const auto arc = std::find_if(view.visible.begin(), view.visible.end(),
                                      [](const ProjectedCurve& c) { return c.isArc; });
        REQUIRE(arc != view.visible.end());
        CHECK_THAT(arc->radius, WithinAbs(10.0, 1e-6));
        CHECK_THAT(arc->endAngle - arc->startAngle, WithinAbs(pi / 2.0, 1e-6));
        // Its points run on the circle, within the angles it says.
        for (const Point2D& p : arc->points) {
            CHECK_THAT(std::hypot(p.x - arc->center.x, p.y - arc->center.y), WithinAbs(10.0, 0.03));
            double angle = std::atan2(p.y - arc->center.y, p.x - arc->center.x);
            while (angle < arc->startAngle - 1e-6) {
                angle += 2.0 * pi;
            }
            CHECK(angle <= arc->endAngle + 1e-6);
        }
    }
}

TEST_CASE("A section keeps what is behind the plane and gives the cut faces",
          "[kernel][projection]") {
    const Shape plate = plateWithHole().shape;
    // Through the hole, looked at from the front: the viewer is on the side the normal points to.
    const Plane through = makePlane({0.0, 20.0, 0.0}, {0.0, -1.0, 0.0}, {1.0, 0.0, 0.0});
    const SectionCut section = sectionBy(plate, through, front);
    CHECK_THAT(volume(section.behind), WithinAbs(volume(plate) / 2.0, 1e-6));
    // Two rectangles of 25 by 10, either side of the hole.
    REQUIRE(section.regions.size() == 2);
    for (const FlatRegion& region : section.regions) {
        CHECK(region.holes.empty());
        REQUIRE(region.outer.size() == 4);
        double area = 0.0;
        for (std::size_t i = 0; i < 4; ++i) {
            const Point2D& a = region.outer[i];
            const Point2D& b = region.outer[(i + 1) % 4];
            area += a.x * b.y - b.x * a.y;
        }
        CHECK_THAT(std::abs(area) / 2.0, WithinAbs(250.0, 1e-6));
    }

    const Plane beside = makePlane({0.0, 90.0, 0.0}, {0.0, -1.0, 0.0}, {1.0, 0.0, 0.0});
    CHECK_THROWS_AS(sectionBy(plate, beside, front), Error);
}

TEST_CASE("The outline of a face has its holes apart", "[kernel][projection]") {
    const NamedShape plate = plateWithHole();
    int found = 0;
    for (int i = 0; i < subShapeCount(plate.shape, SubShapeKind::Face); ++i) {
        const Shape face = subShape(plate.shape, SubShapeKind::Face, i);
        const auto plane = planeOfFace(face);
        if (!plane.has_value() || plane->normal.z < 0.5) {
            continue;
        }
        const FlatRegion region = flatOutline(face, *plane);
        CHECK(region.outer.size() == 4);
        REQUIRE(region.holes.size() == 1);
        // Round, of radius 5, about a center all its points are as far from.
        double sumU = 0.0;
        double sumV = 0.0;
        for (const Point2D& p : region.holes[0]) {
            sumU += p.x;
            sumV += p.y;
        }
        const auto n = static_cast<double>(region.holes[0].size());
        for (const Point2D& p : region.holes[0]) {
            CHECK_THAT(std::hypot(p.x - sumU / n, p.y - sumV / n), WithinAbs(5.0, 0.05));
        }
        ++found;
    }
    CHECK(found == 1);
    CHECK_THROWS_AS(flatOutline(Shape(), planeXY()), Error);
}

TEST_CASE("Edges are described under their names", "[kernel][projection]") {
    const NamedShape plate = plateWithHole();
    const std::vector<EdgeSummary> edges = edgeSummaries(plate);
    CHECK(edges.size() == edgeNames(plate).size());
    int circles = 0;
    int lines = 0;
    for (const EdgeSummary& edge : edges) {
        CHECK_FALSE(edge.name.empty());
        lines += edge.isLine ? 1 : 0;
        if (edge.isCircle) {
            ++circles;
            CHECK_THAT(edge.radius, WithinAbs(5.0, 1e-9));
            CHECK_THAT(edge.center.x, WithinAbs(30.0, 1e-9));
            CHECK_THAT(edge.center.y, WithinAbs(20.0, 1e-9));
            CHECK_THAT(std::abs(edge.axis.z), WithinAbs(1.0, 1e-9));
        }
    }
    CHECK(circles == 2);
    CHECK(lines >= 12);
}
