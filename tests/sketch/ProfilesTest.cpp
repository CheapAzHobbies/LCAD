// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/sketch/Edit.h>
#include <lcad/sketch/Sketch.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

using namespace lcad::sketch;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

const double pi = 3.14159265358979323846;

// A rectangle of four lines, drawn counter-clockwise from its lower left corner.
std::vector<EntityId> addRectangle(Sketch& sketch, double x, double y, double w, double h) {
    return {sketch.addLine({x, y}, {x + w, y}), sketch.addLine({x + w, y}, {x + w, y + h}),
            sketch.addLine({x + w, y + h}, {x, y + h}), sketch.addLine({x, y + h}, {x, y})};
}

std::vector<EntityId> sorted(std::vector<EntityId> ids) {
    std::sort(ids.begin(), ids.end());
    return ids;
}

} // namespace

TEST_CASE("A rectangle is one outline of its area", "[sketch][profiles]") {
    Sketch sketch;
    const std::vector<EntityId> lines = addRectangle(sketch, 2.0, 3.0, 40.0, 25.0);

    const std::vector<Profile> profiles = sketch.profiles();

    REQUIRE(profiles.size() == 1);
    CHECK(sorted(profiles[0].curves) == sorted(lines));
    CHECK_THAT(profiles[0].area, WithinRel(1000.0, 1e-12));
}

TEST_CASE("The direction the lines were drawn in does not matter", "[sketch][profiles]") {
    Sketch sketch;
    // Clockwise, and with one line drawn backwards.
    sketch.addLine({0.0, 0.0}, {0.0, 10.0});
    sketch.addLine({0.0, 10.0}, {20.0, 10.0});
    sketch.addLine({20.0, 0.0}, {20.0, 10.0});
    sketch.addLine({20.0, 0.0}, {0.0, 0.0});

    const std::vector<Profile> profiles = sketch.profiles();

    REQUIRE(profiles.size() == 1);
    CHECK(profiles[0].curves.size() == 4);
    CHECK_THAT(profiles[0].area, WithinRel(200.0, 1e-12));
}

TEST_CASE("A circle is an outline by itself", "[sketch][profiles]") {
    Sketch sketch;
    const EntityId circle = sketch.addCircle({5.0, 5.0}, 3.0);

    const std::vector<Profile> profiles = sketch.profiles();

    REQUIRE(profiles.size() == 1);
    CHECK(profiles[0].curves == std::vector<EntityId>{circle});
    CHECK_THAT(profiles[0].area, WithinRel(9.0 * pi, 1e-12));
}

TEST_CASE("Outlines made with arcs have the arcs' area", "[sketch][profiles]") {
    Sketch sketch;

    SECTION("a half disc: a line and a half circle") {
        sketch.addLine({-4.0, 0.0}, {4.0, 0.0});
        sketch.addArc({0.0, 0.0}, {4.0, 0.0}, {-4.0, 0.0});
        const std::vector<Profile> profiles = sketch.profiles();
        REQUIRE(profiles.size() == 1);
        CHECK_THAT(profiles[0].area, WithinRel(8.0 * pi, 1e-12));
    }

    SECTION("a slot: two lines and two half circles") {
        sketch.addLine({0.0, -3.0}, {20.0, -3.0});
        sketch.addArc({20.0, 0.0}, {20.0, -3.0}, {20.0, 3.0});
        sketch.addLine({20.0, 3.0}, {0.0, 3.0});
        sketch.addArc({0.0, 0.0}, {0.0, 3.0}, {0.0, -3.0});
        const std::vector<Profile> profiles = sketch.profiles();
        REQUIRE(profiles.size() == 1);
        CHECK(profiles[0].curves.size() == 4);
        CHECK_THAT(profiles[0].area, WithinRel(20.0 * 6.0 + 9.0 * pi, 1e-12));
    }

    SECTION("a rectangle with a round bite out of one side") {
        // The arc bulges into the rectangle, so its sliver is taken away, not added.
        sketch.addLine({0.0, 0.0}, {10.0, 0.0});
        sketch.addLine({10.0, 0.0}, {10.0, 8.0});
        sketch.addLine({10.0, 8.0}, {7.0, 8.0});
        sketch.addArc({5.0, 8.0}, {3.0, 8.0}, {7.0, 8.0});
        sketch.addLine({3.0, 8.0}, {0.0, 8.0});
        sketch.addLine({0.0, 8.0}, {0.0, 0.0});
        const std::vector<Profile> profiles = sketch.profiles();
        REQUIRE(profiles.size() == 1);
        CHECK_THAT(profiles[0].area, WithinRel(80.0 - 2.0 * pi, 1e-12));
    }
}

TEST_CASE("What is not closed is not an outline", "[sketch][profiles]") {
    Sketch sketch;

    SECTION("an open chain") {
        sketch.addLine({0.0, 0.0}, {10.0, 0.0});
        sketch.addLine({10.0, 0.0}, {10.0, 5.0});
        sketch.addLine({10.0, 5.0}, {0.0, 5.0});
        CHECK(sketch.profiles().empty());
    }
    SECTION("corners that do not quite meet") {
        addRectangle(sketch, 0.0, 0.0, 10.0, 5.0);
        sketch.remove(sketch.entities().back().id);
        sketch.addLine({0.0, 5.0}, {0.0, 0.001});
        CHECK(sketch.profiles().empty());
    }
    SECTION("an empty sketch, and points") {
        sketch.addPoint({1.0, 1.0});
        CHECK(sketch.profiles().empty());
    }
}

TEST_CASE("Construction geometry and loose ends are left out", "[sketch][profiles]") {
    Sketch sketch;
    const std::vector<EntityId> lines = addRectangle(sketch, 0.0, 0.0, 10.0, 5.0);
    // A tail hanging off one corner, and a construction diagonal.
    sketch.addLine({10.0, 5.0}, {15.0, 9.0});
    const EntityId diagonal = sketch.addLine({0.0, 0.0}, {10.0, 5.0});
    sketch.setConstruction(diagonal, true);
    const EntityId guide = sketch.addCircle({5.0, 2.5}, 1.0);
    sketch.setConstruction(guide, true);

    const std::vector<Profile> profiles = sketch.profiles();

    REQUIRE(profiles.size() == 1);
    CHECK(sorted(profiles[0].curves) == sorted(lines));
    CHECK_THAT(profiles[0].area, WithinRel(50.0, 1e-12));
}

TEST_CASE("Regions that share a side are separate outlines", "[sketch][profiles]") {
    Sketch sketch;
    // Two rooms side by side, with the wall between them drawn once.
    sketch.addLine({0.0, 0.0}, {10.0, 0.0});
    sketch.addLine({10.0, 0.0}, {25.0, 0.0});
    sketch.addLine({25.0, 0.0}, {25.0, 8.0});
    sketch.addLine({25.0, 8.0}, {10.0, 8.0});
    sketch.addLine({10.0, 8.0}, {0.0, 8.0});
    sketch.addLine({0.0, 8.0}, {0.0, 0.0});
    const EntityId wall = sketch.addLine({10.0, 0.0}, {10.0, 8.0});

    const std::vector<Profile> profiles = sketch.profiles();

    REQUIRE(profiles.size() == 2);
    std::vector<double> areas{profiles[0].area, profiles[1].area};
    std::sort(areas.begin(), areas.end());
    CHECK_THAT(areas[0], WithinRel(80.0, 1e-12));
    CHECK_THAT(areas[1], WithinRel(120.0, 1e-12));
    for (const Profile& profile : profiles) {
        CHECK(profile.curves.size() == 4);
        CHECK(std::count(profile.curves.begin(), profile.curves.end(), wall) == 1);
    }
}

TEST_CASE("Separate figures each give their outline", "[sketch][profiles]") {
    Sketch sketch;
    addRectangle(sketch, 0.0, 0.0, 10.0, 10.0);
    addRectangle(sketch, 50.0, 0.0, 4.0, 5.0);
    sketch.addCircle({100.0, 0.0}, 1.0);

    const std::vector<Profile> profiles = sketch.profiles();

    REQUIRE(profiles.size() == 3);
    CHECK_THAT(profiles[0].area, WithinRel(100.0, 1e-12));
    CHECK_THAT(profiles[1].area, WithinRel(20.0, 1e-12));
    CHECK_THAT(profiles[2].area, WithinRel(pi, 1e-12));
}

TEST_CASE("Outlines inside outlines make regions with holes", "[sketch][profiles]") {
    Sketch sketch;
    // A plate 100 by 60 with two round holes, and a square inside one more circle.
    const auto plate = addRectangle(sketch, {0.0, 0.0}, {100.0, 60.0});
    const EntityId first = sketch.addCircle({20.0, 30.0}, 8.0);
    const EntityId second = sketch.addCircle({45.0, 30.0}, 5.0);
    const EntityId ring = sketch.addCircle({78.0, 30.0}, 18.0);
    const auto square = addRectangle(sketch, {70.0, 22.0}, {86.0, 38.0});

    const std::vector<Region> regions = sketch.regions();
    REQUIRE(regions.size() == 5);
    // In the order of profiles(): by the lowest curve of each outline.
    const Region& plateRegion = regions[0];
    CHECK(plateRegion.outer.size() == 4);
    CHECK(std::find(plateRegion.outer.begin(), plateRegion.outer.end(), plate.bottom) !=
          plateRegion.outer.end());
    // Its holes are the three circles. The square is a hole of the ring, not of the plate.
    REQUIRE(plateRegion.holes.size() == 3);
    CHECK_THAT(plateRegion.area, WithinAbs(6000.0 - pi * (64.0 + 25.0 + 324.0), 1e-6));

    const auto regionOf = [&regions](EntityId curve) -> const Region& {
        for (const Region& region : regions) {
            if (std::find(region.outer.begin(), region.outer.end(), curve) != region.outer.end()) {
                return region;
            }
        }
        throw std::logic_error("no region has that curve as its outline");
    };
    CHECK(regionOf(first).holes.empty());
    CHECK_THAT(regionOf(first).area, WithinAbs(pi * 64.0, 1e-6));
    CHECK(regionOf(second).holes.empty());
    const Region& ringRegion = regionOf(ring);
    REQUIRE(ringRegion.holes.size() == 1);
    CHECK(ringRegion.holes[0].size() == 4);
    CHECK_THAT(ringRegion.area, WithinAbs(pi * 324.0 - 256.0, 1e-6));
    CHECK(regionOf(square.bottom).holes.empty());
    CHECK_THAT(regionOf(square.bottom).area, WithinAbs(256.0, 1e-6));
}

TEST_CASE("Outlines that share a side are neighbors, not one inside the other",
          "[sketch][profiles]") {
    Sketch sketch;
    // Two rooms with a wall between them: a 60 by 40 rectangle and a line down its middle.
    addRectangle(sketch, {0.0, 0.0}, {60.0, 40.0});
    Sketch divided = sketch;
    // Split the top and bottom at x = 20 by drawing them again as two lines each.
    Sketch rooms;
    rooms.addLine({0.0, 0.0}, {20.0, 0.0});
    rooms.addLine({20.0, 0.0}, {60.0, 0.0});
    rooms.addLine({60.0, 0.0}, {60.0, 40.0});
    rooms.addLine({60.0, 40.0}, {20.0, 40.0});
    rooms.addLine({20.0, 40.0}, {0.0, 40.0});
    rooms.addLine({0.0, 40.0}, {0.0, 0.0});
    rooms.addLine({20.0, 0.0}, {20.0, 40.0});
    const std::vector<Region> regions = rooms.regions();
    REQUIRE(regions.size() == 2);
    CHECK(regions[0].holes.empty());
    CHECK(regions[1].holes.empty());
    CHECK_THAT(regions[0].area + regions[1].area, WithinAbs(2400.0, 1e-6));
    CHECK_THAT(std::min(regions[0].area, regions[1].area), WithinAbs(800.0, 1e-6));
    CHECK(divided.regions().size() == 1);

    // Nor is an outline that lies partly inside another and partly outside it: a circle
    // centered on the rectangle's side is half in, half out, and is a hole of nothing.
    divided.addCircle({60.0, 20.0}, 5.0);
    const std::vector<Region> crossing = divided.regions();
    REQUIRE(crossing.size() == 2);
    CHECK(crossing[0].holes.empty());
    CHECK(crossing[1].holes.empty());
    CHECK_THAT(crossing[0].area, WithinAbs(2400.0, 1e-6));
}

TEST_CASE("An outline is given as the points around it", "[sketch][profiles]") {
    Sketch sketch;
    const SlotParts slot = addSlot(sketch, {0.0, 0.0}, {30.0, 0.0}, 6.0);
    const std::vector<Profile> outlines = sketch.profiles();
    REQUIRE(outlines.size() == 1);
    const std::vector<Point2> polygon = sketch.outlinePolygon(outlines[0].curves);
    REQUIRE(polygon.size() > 60);
    // The polygon encloses nearly the slot's area (chords cut the arcs a little short), and
    // every point of it is on the slot's outline.
    double twice = 0.0;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Point2 a = polygon[i];
        const Point2 b = polygon[(i + 1) % polygon.size()];
        twice += a.x * b.y - b.x * a.y;
    }
    CHECK_THAT(std::abs(twice) / 2.0, WithinAbs(30.0 * 12.0 + pi * 36.0, 0.2));
    for (const Point2 point : polygon) {
        double nearest = 1e9;
        for (const EntityId curve :
             {slot.firstArc, slot.firstLine, slot.secondArc, slot.secondLine}) {
            nearest = std::min(nearest, distanceTo(sketch, curve, point));
        }
        CHECK(nearest < 1e-9);
    }
    CHECK(sketch.outlinePolygon({}).empty());
}
