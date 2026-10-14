// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Error.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/Plane.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/kernel/Transform.h>

#include <Tolerances.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace lcad::kernel;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

const double pi = 3.14159265358979323846;

// The outline of a rectangle from (x, y) to (x + w, y + h) on a plane, with its four sides
// named "1" to "4": bottom, right, top, left.
std::vector<OutlineCurve> rectangle(const Plane& plane, double x, double y, double w, double h,
                                    int firstName = 1) {
    const auto at = [&plane](double u, double v) { return toWorld(plane, u, v); };
    return {{makeSegment(at(x, y), at(x + w, y)), std::to_string(firstName)},
            {makeSegment(at(x + w, y), at(x + w, y + h)), std::to_string(firstName + 1)},
            {makeSegment(at(x + w, y + h), at(x, y + h)), std::to_string(firstName + 2)},
            {makeSegment(at(x, y + h), at(x, y)), std::to_string(firstName + 3)}};
}

std::vector<OutlineCurve> circle(const Plane& plane, double u, double v, double radius,
                                 const std::string& name) {
    return {{makeCircleEdge(plane, u, v, radius), name}};
}

// A plate of w by h, pulled `thickness` up from the XY plane, as feature "7".
NamedShape plate(double w, double h, double thickness) {
    return pull(planeXY(), {Region{rectangle(planeXY(), 0.0, 0.0, w, h), {}}}, 0.0, thickness, "7");
}

std::set<std::string> asSet(const std::vector<std::string>& names) {
    return {names.begin(), names.end()};
}

// The edges that lie between two side faces of feature "7": the upright ones of a plate.
std::vector<int> uprightEdges(const NamedShape& shape) {
    std::vector<int> found;
    const std::vector<std::string> names = edgeNames(shape);
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::size_t bar = names[i].find('|');
        if (bar != std::string::npos && names[i].compare(0, 7, "7/side/") == 0 &&
            names[i].compare(bar + 1, 7, "7/side/") == 0) {
            found.push_back(static_cast<int>(i));
        }
    }
    return found;
}

} // namespace

TEST_CASE("Pull makes a solid of an outline, and names its faces", "[kernel][modeling]") {
    const NamedShape solid = plate(40.0, 25.0, 10.0);
    CHECK_THAT(volume(solid.shape), WithinRel(10000.0, 1e-9));
    CHECK(countTopology(solid.shape).solids == 1);
    REQUIRE(solid.faceNames.size() == 6);
    REQUIRE(subShapeCount(solid.shape, SubShapeKind::Face) == 6);
    CHECK(asSet(solid.faceNames) == std::set<std::string>{"7/side/1", "7/side/2", "7/side/3",
                                                          "7/side/4", "7/start/1", "7/end/1"});

    // Each name is on the face it describes: the end is the top, at z = 10, with the area of
    // the outline; the side made from the bottom line of the rectangle lies at y = 0.
    const int top = findFace(solid, "7/end/1");
    REQUIRE(top >= 0);
    const Shape topFace = subShape(solid.shape, SubShapeKind::Face, top);
    CHECK_THAT(surfaceArea(topFace), WithinRel(1000.0, 1e-9));
    CHECK_THAT(boundingBox(topFace)->min.z, WithinAbs(10.0, 1e-9));
    const Shape bottomSide = subShape(solid.shape, SubShapeKind::Face, findFace(solid, "7/side/1"));
    CHECK_THAT(surfaceArea(bottomSide), WithinRel(400.0, 1e-9));
    CHECK_THAT(boundingBox(bottomSide)->max.y, WithinAbs(0.0, 1e-9));
    CHECK_THAT(
        boundingBox(subShape(solid.shape, SubShapeKind::Face, findFace(solid, "7/start/1")))->max.z,
        WithinAbs(0.0, 1e-9));
    CHECK(findFace(solid, "7/side/9") == -1);

    // Every edge has a name of its own, made of the two faces it lies between.
    const std::vector<std::string> edges = edgeNames(solid);
    REQUIRE(edges.size() == 12);
    CHECK(asSet(edges).size() == 12);
    const int topFront = findEdge(solid, "7/end/1|7/side/1#0");
    REQUIRE(topFront >= 0);
    const Shape edge = subShape(solid.shape, SubShapeKind::Edge, topFront);
    CHECK_THAT(length(edge), WithinRel(40.0, 1e-9));
    CHECK_THAT(boundingBox(edge)->min.z, WithinAbs(10.0, 1e-9));
    CHECK_THAT(boundingBox(edge)->max.y, WithinAbs(0.0, 1e-9));
    CHECK(findEdge(solid, "7/end/1|7/side/9#0") == -1);
    CHECK(uprightEdges(solid).size() == 4);
}

TEST_CASE("The names do not change when the outline is given another size", "[kernel][modeling]") {
    const NamedShape small = plate(40.0, 25.0, 10.0);
    const NamedShape large = plate(95.5, 31.0, 4.0);
    CHECK(asSet(small.faceNames) == asSet(large.faceNames));
    CHECK(asSet(edgeNames(small)) == asSet(edgeNames(large)));
    // And a name still means the same face: the side made from the rectangle's right side.
    const Shape right = subShape(large.shape, SubShapeKind::Face, findFace(large, "7/side/2"));
    CHECK_THAT(boundingBox(right)->min.x, WithinAbs(95.5, 1e-9));
    CHECK_THAT(surfaceArea(right), WithinRel(31.0 * 4.0, 1e-9));
}

TEST_CASE("Pull goes where it is told, on any plane, around holes", "[kernel][modeling]") {
    SECTION("downward, and to both sides") {
        const Region region{rectangle(planeXY(), 0.0, 0.0, 40.0, 25.0), {}};
        const NamedShape down = pull(planeXY(), {region}, 0.0, -10.0, "7");
        CHECK_THAT(volume(down.shape), WithinRel(10000.0, 1e-9));
        CHECK_THAT(boundingBox(down.shape)->min.z, WithinAbs(-10.0, 1e-9));
        CHECK_THAT(boundingBox(down.shape)->max.z, WithinAbs(0.0, 1e-9));
        // The start is the lower end, whichever way it was asked for.
        CHECK_THAT(
            boundingBox(subShape(down.shape, SubShapeKind::Face, findFace(down, "7/start/1")))
                ->max.z,
            WithinAbs(-10.0, 1e-9));

        const NamedShape both = pull(planeXY(), {region}, -3.0, 7.0, "7");
        CHECK_THAT(volume(both.shape), WithinRel(10000.0, 1e-9));
        CHECK_THAT(boundingBox(both.shape)->min.z, WithinAbs(-3.0, 1e-9));
        CHECK_THAT(boundingBox(both.shape)->max.z, WithinAbs(7.0, 1e-9));
    }
    SECTION("on the front plane, along its normal") {
        // The XZ plane is looked at from -Y, so pulling it forward goes toward -Y.
        const NamedShape solid = pull(
            planeXZ(), {Region{rectangle(planeXZ(), 0.0, 0.0, 40.0, 25.0), {}}}, 0.0, 10.0, "7");
        CHECK_THAT(volume(solid.shape), WithinRel(10000.0, 1e-9));
        CHECK_THAT(boundingBox(solid.shape)->min.y, WithinAbs(-10.0, 1e-9));
        CHECK_THAT(boundingBox(solid.shape)->max.z, WithinAbs(25.0, 1e-9));
    }
    SECTION("a region with a hole leaves the hole") {
        const Region region{rectangle(planeXY(), 0.0, 0.0, 40.0, 25.0),
                            {circle(planeXY(), 20.0, 12.5, 5.0, "5")}};
        CHECK_THAT(surfaceArea(makeRegionFace(planeXY(), region)),
                   WithinRel(1000.0 - pi * 25.0, 1e-9));
        const NamedShape solid = pull(planeXY(), {region}, 0.0, 10.0, "7");
        CHECK_THAT(volume(solid.shape), WithinRel((1000.0 - pi * 25.0) * 10.0, 1e-9));
        REQUIRE(solid.faceNames.size() == 7);
        // The wall of the hole is named after the circle.
        const int wall = findFace(solid, "7/side/5");
        REQUIRE(wall >= 0);
        CHECK_THAT(surfaceArea(subShape(solid.shape, SubShapeKind::Face, wall)),
                   WithinRel(2.0 * pi * 5.0 * 10.0, 1e-9));
    }
    SECTION("regions that share a side become one solid") {
        const Region left{rectangle(planeXY(), 0.0, 0.0, 20.0, 25.0, 1), {}};
        const Region right{rectangle(planeXY(), 20.0, 0.0, 20.0, 25.0, 11), {}};
        const NamedShape solid = pull(planeXY(), {left, right}, 0.0, 10.0, "7");
        CHECK_THAT(volume(solid.shape), WithinRel(10000.0, 1e-9));
        CHECK(countTopology(solid.shape).solids == 1);
        // No wall between them, and the tops and bottoms have become one face each.
        CHECK(solid.faceNames.size() == 6);
        CHECK(findFace(solid, "7/end/1") >= 0);
        CHECK(asSet(solid.faceNames).size() == solid.faceNames.size());
    }
    SECTION("what cannot be pulled is refused") {
        const Region region{rectangle(planeXY(), 0.0, 0.0, 40.0, 25.0), {}};
        CHECK_THROWS_AS(pull(planeXY(), {region}, 0.0, 0.0, "7"), std::invalid_argument);
        CHECK_THROWS_AS(pull(planeXY(), {}, 0.0, 5.0, "7"), std::invalid_argument);
        // Three sides of a rectangle are no outline.
        Region open = region;
        open.outer.pop_back();
        CHECK_THROWS_AS(pull(planeXY(), {open}, 0.0, 5.0, "7"), Error);
        CHECK_THROWS_AS(makeRegionFace(planeXY(), open), Error);
    }
}

TEST_CASE("Combining solids keeps the names of the faces that survive", "[kernel][modeling]") {
    const NamedShape body = plate(40.0, 25.0, 10.0);
    // A round post standing on the plate, and reaching 5 into it: feature "9".
    const Plane top = makePlane({0.0, 0.0, 10.0}, {0.0, 0.0, 1.0}, {1.0, 0.0, 0.0});
    const NamedShape post =
        pull(top, {Region{circle(top, 20.0, 12.5, 5.0, "3"), {}}}, -5.0, 8.0, "9");

    SECTION("join") {
        const NamedShape joined = combine(body, post, Combination::Join);
        CHECK_THAT(volume(joined.shape), WithinRel(10000.0 + pi * 25.0 * 8.0, 1e-9));
        CHECK(countTopology(joined.shape).solids == 1);
        // The plate's six faces, the post's wall, and the post's top. What was inside is gone.
        CHECK(asSet(joined.faceNames) == std::set<std::string>{"7/side/1", "7/side/2", "7/side/3",
                                                               "7/side/4", "7/start/1", "7/end/1",
                                                               "9/side/3", "9/end/3"});
        // The plate's top keeps its name with a hole cut into it by the post.
        CHECK_THAT(
            surfaceArea(subShape(joined.shape, SubShapeKind::Face, findFace(joined, "7/end/1"))),
            WithinRel(1000.0 - pi * 25.0, 1e-9));
        // The edge where the post meets the plate is the one between those two faces.
        const int foot = findEdge(joined, "7/end/1|9/side/3#0");
        REQUIRE(foot >= 0);
        CHECK_THAT(length(subShape(joined.shape, SubShapeKind::Edge, foot)),
                   WithinRel(2.0 * pi * 5.0, 1e-9));
    }
    SECTION("cut") {
        const NamedShape cut = combine(body, post, Combination::Cut);
        CHECK_THAT(volume(cut.shape), WithinRel(10000.0 - pi * 25.0 * 5.0, 1e-9));
        // A blind hole: its wall is the tool's side, its floor the tool's start.
        CHECK(asSet(cut.faceNames) == std::set<std::string>{"7/side/1", "7/side/2", "7/side/3",
                                                            "7/side/4", "7/start/1", "7/end/1",
                                                            "9/side/3", "9/start/3"});
        CHECK_THAT(surfaceArea(subShape(cut.shape, SubShapeKind::Face, findFace(cut, "9/start/3"))),
                   WithinRel(pi * 25.0, 1e-9));
    }
    SECTION("intersect") {
        const NamedShape common = combine(body, post, Combination::Intersect);
        CHECK_THAT(volume(common.shape), WithinRel(pi * 25.0 * 5.0, 1e-9));
        CHECK(findFace(common, "9/side/3") >= 0);
        CHECK(findFace(common, "7/end/1") >= 0);
        CHECK(findFace(common, "7/side/1") == -1);
    }
    SECTION("a cut that leaves nothing, and solids that do not meet, say so") {
        const NamedShape cover = pull(
            planeXY(), {Region{rectangle(planeXY(), -5.0, -5.0, 50.0, 35.0), {}}}, -1.0, 11.0, "9");
        try {
            (void)combine(body, cover, Combination::Cut);
            FAIL("the cut should have been refused");
        } catch (const Error& failure) {
            CHECK(failure.reason() == "the cut leaves nothing of the body");
        }
        const NamedShape far = pull(
            planeXY(), {Region{rectangle(planeXY(), 100.0, 0.0, 5.0, 5.0), {}}}, 0.0, 5.0, "9");
        try {
            (void)combine(body, far, Combination::Intersect);
            FAIL("the intersection should have been refused");
        } catch (const Error& failure) {
            CHECK(failure.reason() == "the two do not overlap");
        }
        // Joined, they are two solids in one body.
        const NamedShape both = combine(body, far, Combination::Join);
        CHECK(countTopology(both.shape).solids == 2);
        CHECK_THAT(volume(both.shape), WithinRel(10125.0, 1e-9));
    }
    SECTION("faces without a history get numbers") {
        const NamedShape box = nameFaces(makeBox(10.0, 10.0, 10.0), "2/f");
        REQUIRE(box.faceNames.size() == 6);
        CHECK(box.faceNames[0] == "2/f0");
        CHECK(box.faceNames[5] == "2/f5");
        CHECK(asSet(edgeNames(box)).size() == 12);
        CHECK(edgeNames(NamedShape{}).empty());
    }
}

TEST_CASE("Fillet and chamfer find their edges by name after the body has changed",
          "[kernel][modeling]") {
    const NamedShape body = plate(40.0, 25.0, 10.0);
    const std::vector<int> corners = uprightEdges(body);
    REQUIRE(corners.size() == 4);

    SECTION("rounding the four upright edges") {
        const NamedShape rounded = fillet(body, corners, 5.0, "8");
        // Each corner loses a square of side r less a quarter disc of radius r.
        CHECK_THAT(volume(rounded.shape), WithinRel((1000.0 - (4.0 - pi) * 25.0) * 10.0, 1e-9));
        // The six faces it had, and four new ones named after the edges they replace.
        REQUIRE(rounded.faceNames.size() == 10);
        int made = 0;
        for (const std::string& name : rounded.faceNames) {
            made += name.compare(0, 7, "8/edge/") == 0 ? 1 : 0;
        }
        CHECK(made == 4);
        for (const char* kept :
             {"7/side/1", "7/side/2", "7/side/3", "7/side/4", "7/start/1", "7/end/1"}) {
            CHECK(findFace(rounded, kept) >= 0);
        }
        CHECK(asSet(rounded.faceNames).size() == 10);
    }
    SECTION("the same edges, by the same names, on a plate of another size") {
        std::vector<std::string> names;
        const std::vector<std::string> all = edgeNames(body);
        for (const int edge : corners) {
            names.push_back(all[static_cast<std::size_t>(edge)]);
        }
        const NamedShape larger = plate(90.0, 60.0, 4.0);
        std::vector<int> found;
        for (const std::string& name : names) {
            const int edge = findEdge(larger, name);
            REQUIRE(edge >= 0);
            // An upright edge, as long as the plate is thick.
            CHECK_THAT(length(subShape(larger.shape, SubShapeKind::Edge, edge)),
                       WithinRel(4.0, 1e-9));
            found.push_back(edge);
        }
        const NamedShape rounded = fillet(larger, found, 5.0, "8");
        CHECK_THAT(volume(rounded.shape), WithinRel((5400.0 - (4.0 - pi) * 25.0) * 4.0, 1e-9));
        // The new faces have the names they had on the smaller plate.
        CHECK(asSet(rounded.faceNames) == asSet(fillet(body, corners, 5.0, "8").faceNames));
    }
    SECTION("beveling one edge") {
        const NamedShape beveled = chamfer(body, {corners.front()}, 3.0, "8");
        CHECK_THAT(volume(beveled.shape), WithinRel(10000.0 - 4.5 * 10.0, 1e-9));
        CHECK(beveled.faceNames.size() == 7);
    }
    SECTION("what does not fit is refused, with the reason") {
        try {
            (void)fillet(body, corners, 13.0, "8");
            FAIL("a radius of more than half the plate's width should not fit");
        } catch (const Error& failure) {
            CHECK(failure.reason() == "the radius does not fit these edges");
        }
        CHECK_THROWS_AS(chamfer(body, corners, 30.0, "8"), Error);
        CHECK_THROWS_AS(fillet(body, {}, 2.0, "8"), std::invalid_argument);
        CHECK_THROWS_AS(fillet(body, corners, 0.0, "8"), std::invalid_argument);
        CHECK_THROWS_AS(fillet(body, {99}, 2.0, "8"), std::out_of_range);
    }
}

TEST_CASE("Shell hollows a body out", "[kernel][modeling]") {
    const NamedShape body = plate(40.0, 25.0, 10.0);
    const NamedShape hollow = shell(body, {findFace(body, "7/end/1")}, 2.0, "8");
    // A tray: the outside as it was, less the inside, 36 by 21 and 8 deep.
    CHECK_THAT(volume(hollow.shape), WithinRel(10000.0 - 36.0 * 21.0 * 8.0, 1e-9));
    // The outside keeps its names. What is left of the top that was opened, the rim of the
    // tray, still goes by the top's name.
    CHECK(findFace(hollow, "7/side/1") >= 0);
    CHECK(findFace(hollow, "7/start/1") >= 0);
    const int rim = findFace(hollow, "7/end/1");
    REQUIRE(rim >= 0);
    CHECK_THAT(surfaceArea(subShape(hollow.shape, SubShapeKind::Face, rim)),
               WithinRel(1000.0 - 36.0 * 21.0, 1e-9));
    CHECK(asSet(hollow.faceNames).size() == hollow.faceNames.size());
    int inner = 0;
    for (const std::string& name : hollow.faceNames) {
        inner += name.compare(0, 8, "8/inner/") == 0 ? 1 : 0;
    }
    CHECK(inner == 5);
    CHECK_THROWS_AS(shell(body, {findFace(body, "7/end/1")}, 20.0, "8"), Error);
    // A quarter ring 10 wide and 6 high, opened at one end, cannot have walls 20 thick. Some
    // versions of OCCT return a solid for it anyway, which is refused.
    const NamedShape ring =
        revolve(planeXZ(), {Region{rectangle(planeXZ(), 20.0, 0.0, 10.0, 6.0), {}}}, 0.0, -10.0,
                0.0, 30.0, pi / 2.0, "9");
    const int opening = findFace(ring, "9/start/1");
    REQUIRE(opening >= 0);
    CHECK_NOTHROW(shell(ring, {opening}, 1.0, "8"));
    CHECK_THROWS_AS(shell(ring, {opening}, 20.0, "8"), Error);
    CHECK_THROWS_AS(shell(body, {99}, 2.0, "8"), std::out_of_range);
    CHECK_THROWS_AS(shell(body, {}, 0.0, "8"), std::invalid_argument);
}

TEST_CASE("Revolve turns an outline about an axis in its plane", "[kernel][modeling]") {
    // A 10 by 6 rectangle whose middle is 25 from the Y axis of the XY plane.
    const Region region{rectangle(planeXY(), 20.0, 0.0, 10.0, 6.0), {}};

    const NamedShape ring = revolve(planeXY(), {region}, 0.0, 0.0, 0.0, 1.0, 2.0 * pi, "7");
    // Pappus: the area times the distance its middle travels.
    CHECK_THAT(volume(ring.shape), WithinRel(60.0 * 2.0 * pi * 25.0, 1e-9));
    CHECK(countTopology(ring.shape).solids == 1);
    // Four faces, one from each side of the rectangle, and no ends.
    CHECK(asSet(ring.faceNames) ==
          std::set<std::string>{"7/side/1", "7/side/2", "7/side/3", "7/side/4"});

    const NamedShape quarter = revolve(planeXY(), {region}, 0.0, 0.0, 0.0, 1.0, pi / 2.0, "7");
    CHECK_THAT(volume(quarter.shape), WithinRel(60.0 * 2.0 * pi * 25.0 / 4.0, 1e-9));
    CHECK(findFace(quarter, "7/start/1") >= 0);
    CHECK(findFace(quarter, "7/end/1") >= 0);
    CHECK(quarter.faceNames.size() == 6);

    CHECK_THROWS_AS(revolve(planeXY(), {region}, 0.0, 0.0, 0.0, 0.0, pi, "7"),
                    std::invalid_argument);
    CHECK_THROWS_AS(revolve(planeXY(), {region}, 0.0, 0.0, 0.0, 1.0, 0.0, "7"),
                    std::invalid_argument);
    CHECK_THROWS_AS(revolve(planeXY(), {}, 0.0, 0.0, 0.0, 1.0, pi, "7"), std::invalid_argument);
}

namespace {

// The middle of each face of a shape, in the order the faces are numbered.
std::vector<Point> faceMiddles(const Shape& shape) {
    std::vector<Point> middles;
    for (int i = 0; i < subShapeCount(shape, SubShapeKind::Face); ++i) {
        const auto box = boundingBox(subShape(shape, SubShapeKind::Face, i));
        REQUIRE(box.has_value());
        middles.push_back({(box->min.x + box->max.x) / 2.0, (box->min.y + box->max.y) / 2.0,
                           (box->min.z + box->max.z) / 2.0});
    }
    return middles;
}

} // namespace

TEST_CASE("Copies keep their faces' names, behind a prefix", "[kernel][modeling]") {
    const NamedShape original = plate(60.0, 40.0, 8.0);
    const std::vector<Point> before = faceMiddles(original.shape);

    SECTION("moved") {
        const NamedShape copy = moved(original, {100.0, 0.0, 5.0}, "9/1");
        CHECK(isValid(copy.shape));
        CHECK_THAT(volume(copy.shape), WithinRel(60.0 * 40.0 * 8.0, 1e-9));
        REQUIRE(copy.faceNames.size() == original.faceNames.size());
        const std::vector<Point> after = faceMiddles(copy.shape);
        for (std::size_t i = 0; i < copy.faceNames.size(); ++i) {
            // The face of that name is the copy of the face of the original's name.
            const int j = findFace(original, copy.faceNames[i].substr(4));
            REQUIRE(j >= 0);
            CHECK(copy.faceNames[i].rfind("9/1/", 0) == 0);
            CHECK_THAT(after[i].x, WithinAbs(before[static_cast<std::size_t>(j)].x + 100.0, 1e-6));
            CHECK_THAT(after[i].z, WithinAbs(before[static_cast<std::size_t>(j)].z + 5.0, 1e-6));
        }
        // The original is untouched.
        CHECK(faceMiddles(original.shape)[0].x == before[0].x);
    }

    SECTION("turned") {
        // A quarter turn about the Z axis: X goes to Y.
        const NamedShape copy =
            turned(original, Axis{{0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}}, pi / 2.0, "9/1");
        CHECK(isValid(copy.shape));
        const auto box = boundingBox(copy.shape);
        REQUIRE(box.has_value());
        CHECK_THAT(box->min.x, WithinAbs(-40.0, 1e-6));
        CHECK_THAT(box->max.y, WithinAbs(60.0, 1e-6));
        const std::vector<Point> after = faceMiddles(copy.shape);
        for (std::size_t i = 0; i < copy.faceNames.size(); ++i) {
            const auto j =
                static_cast<std::size_t>(findFace(original, copy.faceNames[i].substr(4)));
            CHECK_THAT(after[i].x, WithinAbs(-before[j].y, 1e-6));
            CHECK_THAT(after[i].y, WithinAbs(before[j].x, 1e-6));
        }
    }

    SECTION("mirrored") {
        // In the YZ plane: X changes sign. The image is a proper solid, not inside out.
        const NamedShape copy = mirrored(original, planeYZ(), "9/1");
        CHECK(isValid(copy.shape));
        CHECK_THAT(volume(copy.shape), WithinRel(60.0 * 40.0 * 8.0, 1e-9));
        const std::vector<Point> after = faceMiddles(copy.shape);
        for (std::size_t i = 0; i < copy.faceNames.size(); ++i) {
            const auto j =
                static_cast<std::size_t>(findFace(original, copy.faceNames[i].substr(4)));
            CHECK_THAT(after[i].x, WithinAbs(-before[j].x, 1e-6));
            CHECK_THAT(after[i].y, WithinAbs(before[j].y, 1e-6));
        }
        // Its flat faces face out of it, as the original's do: the image of the end at x = 60
        // faces toward -X.
        for (int i = 0; i < subShapeCount(copy.shape, SubShapeKind::Face); ++i) {
            const auto plane = planeOfFace(subShape(copy.shape, SubShapeKind::Face, i));
            REQUIRE(plane.has_value());
            const Point middle = after[static_cast<std::size_t>(i)];
            if (std::abs(middle.x + 60.0) < 1e-6) {
                CHECK_THAT(plane->normal.x, WithinAbs(-1.0, 1e-9));
            }
            if (std::abs(middle.z - 8.0) < 1e-6) {
                CHECK_THAT(plane->normal.z, WithinAbs(1.0, 1e-9));
            }
        }
        // It joins its original along the plane into one block.
        const NamedShape both = combine(original, copy, Combination::Join);
        CHECK_THAT(volume(both.shape), WithinRel(2.0 * 60.0 * 40.0 * 8.0, 1e-9));
        CHECK(countTopology(both.shape).solids == 1);
    }

    CHECK_THROWS_AS(moved(NamedShape{}, {1.0, 0.0, 0.0}, "9"), std::invalid_argument);
    CHECK_THROWS_AS(turned(original, Axis{{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}}, 1.0, "9"),
                    std::invalid_argument);
}

TEST_CASE("Edges and faces stand for axes", "[kernel][modeling]") {
    // A cylinder of radius 5 and height 10 standing at (20, 0, 0).
    const Shape rod = translated(makeCylinder(5.0, 10.0), 20.0, 0.0, 0.0);
    int round = 0;
    int straight = 0;
    for (int i = 0; i < subShapeCount(rod, SubShapeKind::Edge); ++i) {
        const Shape edge = subShape(rod, SubShapeKind::Edge, i);
        const auto axis = axisOf(edge);
        REQUIRE(axis.has_value());
        if (std::abs(axis->direction.z) > 0.999) {
            // The rims are circles about the rod's axis, and the seam runs along it.
            if (std::abs(axis->origin.x - 20.0) < 1e-6 && std::abs(axis->origin.y) < 1e-6) {
                ++round;
            } else {
                ++straight;
            }
        }
    }
    CHECK(round == 2);
    CHECK(straight == 1);
    int sides = 0;
    int ends = 0;
    for (int i = 0; i < subShapeCount(rod, SubShapeKind::Face); ++i) {
        const auto axis = axisOf(subShape(rod, SubShapeKind::Face, i));
        REQUIRE(axis.has_value());
        CHECK_THAT(std::abs(axis->direction.z), WithinAbs(1.0, 1e-9));
        CHECK_THAT(axis->origin.x, WithinAbs(20.0, 1e-6));
        CHECK_THAT(axis->origin.y, WithinAbs(0.0, 1e-6));
        // A flat end's axis goes through its middle, and points out of the rod.
        if (std::abs(axis->origin.z - 10.0) < 1e-6) {
            CHECK(axis->direction.z > 0.0);
            ++ends;
        } else if (std::abs(axis->origin.z) < 1e-6 && axis->direction.z < 0.0) {
            ++ends;
        } else {
            ++sides;
        }
    }
    CHECK(ends == 2);
    CHECK(sides == 1);
    CHECK_FALSE(axisOf(Shape()).has_value());
    CHECK_FALSE(axisOf(rod).has_value());
}

TEST_CASE("Holes: simple, counterbored, countersunk, and through", "[kernel][modeling]") {
    const Point top{10.0, 10.0, 8.0};
    const Vector down{0.0, 0.0, -1.0};
    const double r = 3.0;
    HoleShape simple;
    simple.diameter = 2.0 * r;
    simple.depth = 5.0;
    simple.tipAngle = 118.0;
    const double tip = r / std::tan(59.0 * pi / 180.0);

    // Drilled to 5 with a drill's point: a cylinder and a cone.
    const NamedShape drilled = holeTool(top, down, simple, 0.0, "9", "4");
    CHECK(isValid(drilled.shape));
    CHECK_THAT(volume(drilled.shape), WithinRel(pi * r * r * 5.0 + pi * r * r * tip / 3.0, 1e-6));
    auto box = boundingBox(drilled.shape);
    REQUIRE(box.has_value());
    CHECK_THAT(box->max.z, WithinAbs(8.0, 1e-6));
    CHECK_THAT(box->min.z, WithinAbs(3.0 - tip, 1e-6));
    CHECK_THAT(box->min.x, WithinAbs(7.0, 1e-6));
    for (const char* part : {"9/side/bore@4", "9/side/tip@4", "9/side/top@4"}) {
        CHECK(findFace(drilled, part) >= 0);
    }

    // Through, 8 deep: a plain cylinder, flat at the bottom.
    const NamedShape through = holeTool(top, down, simple, 8.0, "9", "4");
    CHECK_THAT(volume(through.shape), WithinRel(pi * r * r * 8.0, 1e-6));
    CHECK(findFace(through, "9/side/floor@4") >= 0);

    // Counterbored: 10 wide for the first 2.
    HoleShape counterbore = simple;
    counterbore.kind = HoleKind::Counterbore;
    counterbore.headDiameter = 10.0;
    counterbore.headDepth = 2.0;
    const NamedShape bored = holeTool(top, down, counterbore, 8.0, "9", "4");
    CHECK_THAT(volume(bored.shape), WithinRel(pi * 25.0 * 2.0 + pi * r * r * 6.0, 1e-6));
    CHECK(findFace(bored, "9/side/counterbore@4") >= 0);
    CHECK(findFace(bored, "9/side/shoulder@4") >= 0);

    // Countersunk at 90 degrees from 10 wide: a cone 2 deep down to the hole's width.
    HoleShape countersink = simple;
    countersink.kind = HoleKind::Countersink;
    countersink.headDiameter = 10.0;
    countersink.countersinkAngle = 90.0;
    const NamedShape sunk = holeTool(top, down, countersink, 8.0, "9", "4");
    const double frustum = pi * 2.0 / 3.0 * (25.0 + 5.0 * r + r * r);
    CHECK_THAT(volume(sunk.shape), WithinRel(frustum + pi * r * r * 6.0, 1e-6));
    CHECK(findFace(sunk, "9/side/countersink@4") >= 0);

    // Through a plate, it takes away exactly that.
    const NamedShape body = plate(40.0, 25.0, 8.0);
    const NamedShape holed = combine(body, through, Combination::Cut);
    CHECK_THAT(volume(holed.shape), WithinRel(40.0 * 25.0 * 8.0 - pi * r * r * 8.0, 1e-9));

    // Sideways works too: along X into the side of the plate.
    const NamedShape side = holeTool({40.0, 12.5, 4.0}, {-1.0, 0.0, 0.0}, simple, 0.0, "9", "5");
    box = boundingBox(side.shape);
    CHECK_THAT(box->max.x, WithinAbs(40.0, 1e-6));
    CHECK_THAT(box->min.x, WithinAbs(35.0 - tip, 1e-6));

    // Sizes that describe no hole are refused.
    HoleShape wrong = simple;
    wrong.diameter = 0.0;
    CHECK_THROWS_AS(holeTool(top, down, wrong, 0.0, "9", "4"), std::invalid_argument);
    wrong = counterbore;
    wrong.headDiameter = 4.0;
    CHECK_THROWS_AS(holeTool(top, down, wrong, 8.0, "9", "4"), std::invalid_argument);
    wrong = counterbore;
    wrong.headDepth = 9.0;
    CHECK_THROWS_AS(holeTool(top, down, wrong, 8.0, "9", "4"), std::invalid_argument);
    CHECK_THROWS_AS(holeTool(top, {0.0, 0.0, 0.0}, simple, 0.0, "9", "4"), std::invalid_argument);
}

TEST_CASE("A pull can taper, and can go up to a face", "[kernel][modeling]") {
    const std::vector<Region> square{Region{rectangle(planeXY(), 0.0, 0.0, 20.0, 20.0), {}}};

    // Tapered 10 degrees over 10 up: the top is narrower by 2·10·tan(10°) each way. A frustum
    // of a square pyramid.
    const double shrink = 10.0 * std::tan(10.0 * pi / 180.0);
    const NamedShape narrowing = pull(planeXY(), square, 0.0, 10.0, "7", 10.0 * pi / 180.0);
    CHECK(isValid(narrowing.shape));
    const double top = 20.0 - 2.0 * shrink;
    CHECK_THAT(volume(narrowing.shape),
               WithinRel(10.0 / 3.0 * (400.0 + top * top + 20.0 * top), 1e-6));
    auto box = boundingBox(narrowing.shape);
    REQUIRE(box.has_value());
    CHECK_THAT(box->max.x, WithinAbs(20.0, 1e-6));
    // The faces keep their names.
    CHECK(findFace(narrowing, "7/side/1") >= 0);
    CHECK(findFace(narrowing, "7/end/1") >= 0);
    CHECK(findFace(narrowing, "7/start/1") >= 0);
    // A negative taper widens, also going down.
    const NamedShape widening = pull(planeXY(), square, -10.0, 0.0, "7", -10.0 * pi / 180.0);
    const double bottom = 20.0 + 2.0 * shrink;
    CHECK_THAT(volume(widening.shape),
               WithinRel(10.0 / 3.0 * (400.0 + bottom * bottom + 20.0 * bottom), 1e-6));
    box = boundingBox(widening.shape);
    CHECK_THAT(box->min.z, WithinAbs(-10.0, 1e-6));
    CHECK_THAT(box->min.x, WithinAbs(-shrink, 1e-6));
    // Both ways at once, or too steep, is refused.
    CHECK_THROWS_AS(pull(planeXY(), square, -5.0, 5.0, "7", 0.1), std::invalid_argument);
    CHECK_THROWS_AS(pull(planeXY(), square, 0.0, 5.0, "7", pi / 2.0), std::invalid_argument);

    // Up to a parallel plane 12 above: exactly that far.
    Plane above = planeXY();
    above.origin = {0.0, 0.0, 12.0};
    NamedShape up = pullTo(planeXY(), square, above, "8");
    CHECK_THAT(volume(up.shape), WithinRel(400.0 * 12.0, 1e-9));
    // Up to a slanted plane, rising from 10 at x = 0 to 30 at x = 20: a wedge on a block.
    Plane slant;
    slant.origin = {0.0, 0.0, 10.0};
    const double len = std::sqrt(2.0);
    slant.normal = {-1.0 / len, 0.0, 1.0 / len};
    slant.xDirection = {1.0 / len, 0.0, 1.0 / len};
    up = pullTo(planeXY(), square, slant, "8");
    CHECK(isValid(up.shape));
    CHECK_THAT(volume(up.shape), WithinRel(20.0 * 20.0 * 20.0, 1e-6));
    box = boundingBox(up.shape);
    CHECK_THAT(box->max.z, WithinAbs(30.0, 1e-6));
    CHECK(findFace(up, "8/end/1") >= 0);
    CHECK(findFace(up, "8/side/2") >= 0);
    // Down to a plane below works the same way.
    Plane below = planeXY();
    below.origin = {0.0, 0.0, -4.0};
    up = pullTo(planeXY(), square, below, "8");
    box = boundingBox(up.shape);
    CHECK_THAT(box->min.z, WithinAbs(-4.0, 1e-9));
    // A plane that runs along the pull, or through the outline, cannot end it.
    CHECK_THROWS_AS(pullTo(planeXY(), square, planeXZ(), "8"), Error);
    Plane through;
    through.origin = {10.0, 0.0, 0.0};
    through.normal = {-1.0 / len, 0.0, 1.0 / len};
    through.xDirection = {1.0 / len, 0.0, 1.0 / len};
    CHECK_THROWS_AS(pullTo(planeXY(), square, through, "8"), Error);
}
