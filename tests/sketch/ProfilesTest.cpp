// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/sketch/Sketch.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <vector>

using namespace lcad::sketch;
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
