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

TEST_CASE("Outlines that cross cut each other into areas", "[sketch][profiles]") {
    SECTION("Two boxes: each without the other, and what they share") {
        Sketch sketch;
        addRectangle(sketch, {0.0, 0.0}, {30.0, 20.0});
        addRectangle(sketch, {20.0, 10.0}, {50.0, 30.0});
        CHECK(sketch.outlineRegions().size() == 2);
        const std::vector<Region> areas = sketch.regions();
        REQUIRE(areas.size() == 3);
        std::vector<double> sizes;
        for (const Region& area : areas) {
            sizes.push_back(area.area);
            CHECK_FALSE(area.pieces.empty());
        }
        std::sort(sizes.begin(), sizes.end());
        CHECK_THAT(sizes[0], WithinAbs(100.0, 1e-6));
        CHECK_THAT(sizes[1], WithinAbs(500.0, 1e-6));
        CHECK_THAT(sizes[2], WithinAbs(500.0, 1e-6));
    }
    SECTION("Two circles, whose three areas are bounded by the same two curves") {
        Sketch sketch;
        sketch.addCircle({0.0, 0.0}, 10.0);
        sketch.addCircle({10.0, 0.0}, 10.0);
        const std::vector<Region> areas = sketch.regions();
        REQUIRE(areas.size() == 3);
        CHECK(areas[0].key != areas[1].key);
        CHECK(areas[1].key != areas[2].key);
        CHECK(areas[0].key != areas[2].key);
        double total = 0.0;
        for (const Region& area : areas) {
            total += area.area;
        }
        // Two discs, less what they share counted twice.
        const double shared = 2.0 * 100.0 * std::acos(0.5) - 5.0 * std::sqrt(400.0 - 100.0);
        CHECK_THAT(total, WithinAbs(2.0 * M_PI * 100.0 - shared, 1e-6));
    }
    SECTION("A cross: five areas, and two with no curve between them are told apart") {
        Sketch sketch;
        addRectangle(sketch, {0.0, 10.0}, {50.0, 20.0});
        addRectangle(sketch, {20.0, 0.0}, {30.0, 30.0});
        const std::vector<Region> areas = sketch.regions();
        REQUIRE(areas.size() == 5);
        for (std::size_t i = 0; i < areas.size(); ++i) {
            for (std::size_t j = i + 1; j < areas.size(); ++j) {
                CHECK(areas[i].key != areas[j].key);
            }
        }
    }
    SECTION("What does not cross stays as it was") {
        Sketch sketch;
        addRectangle(sketch, {0.0, 0.0}, {30.0, 20.0});
        sketch.addCircle({15.0, 10.0}, 5.0);
        addRectangle(sketch, {40.0, 0.0}, {50.0, 10.0});
        const std::vector<Region> areas = sketch.regions();
        CHECK(areas.size() == 3);
        for (const Region& area : areas) {
            CHECK(area.pieces.empty());
        }
    }
    SECTION("An outline inside one of the areas is a hole in that area alone") {
        Sketch sketch;
        addRectangle(sketch, {0.0, 0.0}, {30.0, 20.0});
        addRectangle(sketch, {20.0, 10.0}, {50.0, 30.0});
        sketch.addCircle({8.0, 8.0}, 2.0);
        const std::vector<Region> areas = sketch.regions();
        REQUIRE(areas.size() == 4);
        int withHole = 0;
        for (const Region& area : areas) {
            withHole += area.holes.size() == 1 ? 1 : 0;
        }
        CHECK(withHole == 1);
    }
}

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
    // Each without the other, and what they share.
    REQUIRE(crossing.size() == 3);
    double whole = 0.0;
    for (const Region& region : crossing) {
        CHECK(region.holes.empty());
        whole += region.area;
    }
    CHECK_THAT(whole, WithinAbs(2400.0 + 12.5 * M_PI, 1e-3));
    CHECK(divided.outlineRegions().size() == 2);
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

TEST_CASE("Lines that are in no outline close areas where they cross", "[sketch][profiles]") {
    const auto sizesOf = [](const std::vector<Region>& areas) {
        std::vector<double> sizes;
        for (const Region& area : areas) {
            sizes.push_back(area.area);
        }
        std::sort(sizes.begin(), sizes.end());
        return sizes;
    };
    const auto allApart = [](const std::vector<Region>& areas) {
        for (std::size_t i = 0; i < areas.size(); ++i) {
            for (std::size_t j = i + 1; j < areas.size(); ++j) {
                if (areas[i].key == areas[j].key) {
                    return false;
                }
            }
        }
        return true;
    };
    // The four sides of a 10 by 10 square, each running on 1 past the corners.
    const auto addSquareRunningOn = [](Sketch& sketch, double x, double y, double size) {
        sketch.addLine({x - 1.0, y}, {x + size + 1.0, y});
        sketch.addLine({x + size, y - 1.0}, {x + size, y + size + 1.0});
        sketch.addLine({x + size + 1.0, y + size}, {x - 1.0, y + size});
        sketch.addLine({x, y + size + 1.0}, {x, y - 1.0});
    };

    SECTION("Sides that run on past the corners close the square between them") {
        Sketch sketch;
        addSquareRunningOn(sketch, 0.0, 0.0, 10.0);
        CHECK(sketch.profiles().empty());
        const std::vector<Region> areas = sketch.regions();
        REQUIRE(areas.size() == 1);
        CHECK_THAT(areas[0].area, WithinAbs(100.0, 1e-6));
        CHECK(areas[0].pieces.size() == 4);
        CHECK(areas[0].outer.size() == 4);
    }
    SECTION("So does one side running on past two corners that are joined otherwise") {
        Sketch sketch;
        sketch.addLine({0.0, 0.0}, {10.0, 0.0});
        sketch.addLine({10.0, 0.0}, {10.0, 10.0});
        sketch.addLine({10.0, 10.0}, {0.0, 10.0});
        sketch.addLine({0.0, 12.0}, {0.0, -2.0});
        const std::vector<Region> areas = sketch.regions();
        REQUIRE(areas.size() == 1);
        CHECK_THAT(areas[0].area, WithinAbs(100.0, 1e-6));
    }
    SECTION("A line across a circle cuts it in two") {
        Sketch sketch;
        sketch.addCircle({0.0, 0.0}, 5.0);
        const EntityId line = sketch.addLine({-8.0, 0.0}, {8.0, 0.0});
        const std::vector<Region> areas = sketch.regions();
        REQUIRE(areas.size() == 2);
        CHECK(allApart(areas));
        CHECK_THAT(areas[0].area, WithinAbs(12.5 * pi, 1e-6));
        CHECK_THAT(areas[1].area, WithinAbs(12.5 * pi, 1e-6));
        // Which half is which stays the same as the sketch changes.
        Sketch moved = sketch;
        moved.dragBy(moved.find(line)->end, 1.0, 1.0);
        const std::vector<Region> after = moved.regions();
        REQUIRE(after.size() == 2);
        CHECK(after[0].key == areas[0].key);
        CHECK(after[1].key == areas[1].key);
    }
    SECTION("A circle over the corner of a square whose sides run on: three areas") {
        Sketch sketch;
        addSquareRunningOn(sketch, 0.0, 0.0, 10.0);
        sketch.addCircle({10.0, 10.0}, 3.0);
        const std::vector<Region> areas = sketch.regions();
        REQUIRE(areas.size() == 3);
        CHECK(allApart(areas));
        const std::vector<double> sizes = sizesOf(areas);
        CHECK_THAT(sizes[0], WithinAbs(9.0 * pi / 4.0, 1e-6));
        CHECK_THAT(sizes[1], WithinAbs(27.0 * pi / 4.0, 1e-6));
        CHECK_THAT(sizes[2], WithinAbs(100.0 - 9.0 * pi / 4.0, 1e-6));
    }
    SECTION("A square whose sides run on, inside an outline, is a hole in it") {
        Sketch sketch;
        addRectangle(sketch, {-20.0, -20.0}, {30.0, 30.0});
        addSquareRunningOn(sketch, 0.0, 0.0, 10.0);
        const std::vector<Region> areas = sketch.regions();
        REQUIRE(areas.size() == 2);
        const std::vector<double> sizes = sizesOf(areas);
        CHECK_THAT(sizes[0], WithinAbs(100.0, 1e-6));
        CHECK_THAT(sizes[1], WithinAbs(2500.0 - 100.0, 1e-6));
        const auto outer = std::find_if(areas.begin(), areas.end(),
                                        [](const Region& r) { return r.area > 200.0; });
        CHECK(outer->holes.empty());
        REQUIRE(outer->holePieces.size() == 1);
        CHECK(outer->holePieces[0].size() == 4);
    }
    SECTION("A circle inside such a square is a hole in the square, not in what is around it") {
        Sketch sketch;
        addRectangle(sketch, {-20.0, -20.0}, {30.0, 30.0});
        addSquareRunningOn(sketch, 0.0, 0.0, 10.0);
        sketch.addCircle({5.0, 5.0}, 2.0);
        const std::vector<Region> areas = sketch.regions();
        REQUIRE(areas.size() == 3);
        const std::vector<double> sizes = sizesOf(areas);
        CHECK_THAT(sizes[0], WithinAbs(4.0 * pi, 1e-6));
        CHECK_THAT(sizes[1], WithinAbs(100.0 - 4.0 * pi, 1e-6));
        CHECK_THAT(sizes[2], WithinAbs(2500.0 - 100.0, 1e-6));
    }
    SECTION("Lines that close nothing leave the areas as they were") {
        Sketch sketch;
        const std::vector<EntityId> box = addRectangle(sketch, 0.0, 0.0, 30.0, 20.0);
        sketch.addCircle({50.0, 10.0}, 5.0);
        const std::vector<Region> before = sketch.regions();
        // A line from a corner, a line across nothing, and one from the box to the circle.
        sketch.addLine({30.0, 20.0}, {40.0, 30.0});
        sketch.addLine({-10.0, -10.0}, {-5.0, -12.0});
        sketch.addLine({30.0, 10.0}, {45.0, 10.0});
        const std::vector<Region> after = sketch.regions();
        REQUIRE(after.size() == before.size());
        for (std::size_t i = 0; i < after.size(); ++i) {
            CHECK(after[i].key == before[i].key);
            CHECK(after[i].pieces.empty());
            CHECK(after[i].holePieces.empty());
        }
        CHECK(sorted(after[0].outer) == sorted(box));
    }
    SECTION("A line across a rectangle cuts it in two, and the whole is still known") {
        Sketch sketch;
        const std::vector<EntityId> box = addRectangle(sketch, 0.0, 0.0, 30.0, 20.0);
        const std::vector<Region> before = sketch.regions();
        sketch.addLine({10.0, -5.0}, {10.0, 25.0});
        const std::vector<Region> areas = sketch.regions();
        REQUIRE(areas.size() == 2);
        CHECK(allApart(areas));
        const std::vector<double> sizes = sizesOf(areas);
        CHECK_THAT(sizes[0], WithinAbs(200.0, 1e-6));
        CHECK_THAT(sizes[1], WithinAbs(400.0, 1e-6));
        // A pull of the whole rectangle from before still finds it among the outlines.
        REQUIRE(sketch.outlineRegions().size() == 1);
        CHECK(sketch.outlineRegions()[0].key == before[0].key);
    }
    SECTION("Construction lines cut nothing") {
        Sketch sketch;
        sketch.addCircle({0.0, 0.0}, 5.0);
        const EntityId line = sketch.addLine({-8.0, 0.0}, {8.0, 0.0});
        sketch.setConstruction(line, true);
        CHECK(sketch.regions().size() == 1);
    }
}
