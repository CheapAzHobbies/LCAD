// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Check.h>
#include <lcad/kernel/Error.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/Plane.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/kernel/Transform.h>
#include <lcad/kernel/occt/ShapeAccess.h>

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRep_Builder.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shell.hxx>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <functional>
#include <numbers>
#include <string>
#include <vector>

using namespace lcad::kernel;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double pi = std::numbers::pi;

// The number of the one face that `pick` is true of.
int faceWhere(const NamedShape& body, const std::function<bool(const Shape&)>& pick) {
    int found = -1;
    for (int i = 0; i < subShapeCount(body.shape, SubShapeKind::Face); ++i) {
        if (pick(subShape(body.shape, SubShapeKind::Face, i))) {
            REQUIRE(found < 0);
            found = i;
        }
    }
    REQUIRE(found >= 0);
    return found;
}

// A flat face whose outward normal points along `x`, `y`, `z`, through the point at `at` along
// that axis.
std::function<bool(const Shape&)> flatFacing(double x, double y, double z, double at) {
    return [=](const Shape& face) {
        const auto plane = planeOfFace(face);
        if (!plane.has_value() || std::abs(plane->normal.x * x + plane->normal.y * y +
                                           plane->normal.z * z - 1.0) > 1e-9) {
            return false;
        }
        const Point& o = plane->origin;
        return std::abs(o.x * x + o.y * y + o.z * z - at) < 1e-6;
    };
}

bool isRound(const Shape& face) {
    return radiusOf(face).has_value() && !planeOfFace(face).has_value();
}

// A box 60 by 40 by 10 with a hole of radius 5 through it at (30, 20).
NamedShape plate() {
    const NamedShape box = nameFaces(makeBox(60.0, 40.0, 10.0), "box/f");
    const NamedShape pin =
        nameFaces(translated(makeCylinder(5.0, 12.0), 30.0, 20.0, -1.0), "pin/f");
    return combine(box, pin, Combination::Cut);
}

} // namespace

TEST_CASE("Offset Face moves flat faces along their normals, and resizes round ones",
          "[kernel][direct]") {
    const NamedShape box = nameFaces(makeBox(60.0, 40.0, 10.0), "box/f");
    const int top = faceWhere(box, flatFacing(0, 0, 1, 10.0));
    const std::string topName = box.faceNames[static_cast<std::size_t>(top)];

    const NamedShape up = offsetFaces(box, {top}, 5.0, "o");
    CHECK(isValid(up.shape));
    CHECK_THAT(volume(up.shape), WithinRel(60.0 * 40.0 * 15.0, 1e-9));
    CHECK_THAT(boundingBox(up.shape)->max.z, WithinAbs(15.0, 1e-9));
    // The face moved is the same face, by name, and so are the sides that grew.
    const int moved = findFace(up, topName);
    REQUIRE(moved >= 0);
    CHECK(flatFacing(0, 0, 1, 15.0)(subShape(up.shape, SubShapeKind::Face, moved)));
    for (const std::string& name : box.faceNames) {
        CHECK(findFace(up, name) >= 0);
    }

    const NamedShape down = offsetFaces(box, {top}, -3.0, "o");
    CHECK_THAT(volume(down.shape), WithinRel(60.0 * 40.0 * 7.0, 1e-9));
    // All the way through is no solid.
    CHECK_THROWS_AS(offsetFaces(box, {top}, -10.0, "o"), Error);
    CHECK_THROWS_AS(offsetFaces(box, {top}, 0.0, "o"), std::invalid_argument);
    CHECK_THROWS_AS(offsetFaces(box, {}, 1.0, "o"), std::invalid_argument);

    // The wall of a hole: a negative offset takes material away, so the hole grows.
    const NamedShape holed = plate();
    const int wall = faceWhere(holed, isRound);
    const std::string wallName = holed.faceNames[static_cast<std::size_t>(wall)];
    const NamedShape wider = offsetFaces(holed, {wall}, -2.0, "o");
    CHECK(countTopology(wider.shape).solids == 1);
    CHECK(isValid(wider.shape));
    CHECK_THAT(volume(wider.shape), WithinRel(24000.0 - pi * 49.0 * 10.0, 1e-9));
    const int widerWall = findFace(wider, wallName);
    REQUIRE(widerWall >= 0);
    CHECK_THAT(*radiusOf(subShape(wider.shape, SubShapeKind::Face, widerWall)),
               WithinAbs(7.0, 1e-9));
    const NamedShape narrower = offsetFaces(holed, {wall}, 2.0, "o");
    CHECK_THAT(volume(narrower.shape), WithinRel(24000.0 - pi * 9.0 * 10.0, 1e-9));
}

TEST_CASE("Delete Face takes faces out and closes the gap", "[kernel][direct]") {
    // A hole goes.
    const NamedShape holed = plate();
    const NamedShape filled = removeFaces(holed, {faceWhere(holed, isRound)}, "d");
    CHECK(isValid(filled.shape));
    CHECK_THAT(volume(filled.shape), WithinRel(24000.0, 1e-9));
    CHECK(countTopology(filled.shape).faces == 6);

    // A fillet goes, and the faces it was between meet again, keeping their names.
    const NamedShape box = nameFaces(makeBox(60.0, 40.0, 10.0), "box/f");
    const std::vector<std::string> edges = edgeNames(box);
    const NamedShape rounded = fillet(box, {0}, 4.0, "r");
    REQUIRE(volume(rounded.shape) < 24000.0);
    const NamedShape sharp = removeFaces(rounded, {faceWhere(rounded, isRound)}, "d");
    CHECK_THAT(volume(sharp.shape), WithinRel(24000.0, 1e-9));
    for (const std::string& name : box.faceNames) {
        CHECK(findFace(sharp, name) >= 0);
    }

    // A face whose neighbors cannot close the gap: the top of a box.
    CHECK_THROWS_WITH(removeFaces(box, {faceWhere(box, flatFacing(0, 0, 1, 10.0))}, "d"),
                      ContainsSubstring("cannot be extended to close the gap"));
}

TEST_CASE("Move Face moves flat faces along their normals, and holes and bosses across faces",
          "[kernel][direct]") {
    const NamedShape box = nameFaces(makeBox(60.0, 40.0, 10.0), "box/f");
    const int top = faceWhere(box, flatFacing(0, 0, 1, 10.0));
    // Only the part along the normal counts for a flat face.
    const NamedShape higher = moveFaces(box, {top}, {3.0, 4.0, 5.0}, "m");
    CHECK_THAT(volume(higher.shape), WithinRel(60.0 * 40.0 * 15.0, 1e-9));
    CHECK_THROWS_WITH(moveFaces(box, {top}, {5.0, 0.0, 0.0}, "m"),
                      ContainsSubstring("within their own planes"));

    // A hole moved across the plate.
    const NamedShape holed = plate();
    const int wall = faceWhere(holed, isRound);
    const std::string wallName = holed.faceNames[static_cast<std::size_t>(wall)];
    const NamedShape shifted = moveFaces(holed, {wall}, {15.0, 5.0, 0.0}, "m");
    CHECK(isValid(shifted.shape));
    CHECK_THAT(volume(shifted.shape), WithinRel(volume(holed.shape), 1e-9));
    const int newWall = findFace(shifted, wallName);
    REQUIRE(newWall >= 0);
    const auto axis = axisOf(subShape(shifted.shape, SubShapeKind::Face, newWall));
    REQUIRE(axis.has_value());
    CHECK_THAT(axis->origin.x, WithinAbs(45.0, 1e-9));
    CHECK_THAT(axis->origin.y, WithinAbs(25.0, 1e-9));

    // A round boss moved along the top, its side and top faces together.
    const NamedShape boss =
        nameFaces(translated(makeCylinder(4.0, 6.0), 15.0, 20.0, 10.0), "boss/f");
    const NamedShape bossed = combine(box, boss, Combination::Join);
    std::vector<int> bossFaces;
    for (int i = 0; i < subShapeCount(bossed.shape, SubShapeKind::Face); ++i) {
        if (bossed.faceNames[static_cast<std::size_t>(i)].rfind("boss/", 0) == 0) {
            bossFaces.push_back(i);
        }
    }
    REQUIRE(bossFaces.size() == 2);
    const NamedShape along = moveFaces(bossed, bossFaces, {20.0, 0.0, 0.0}, "m");
    CHECK(isValid(along.shape));
    CHECK_THAT(volume(along.shape), WithinRel(volume(bossed.shape), 1e-9));
    const int side = faceWhere(along, isRound);
    CHECK_THAT(axisOf(subShape(along.shape, SubShapeKind::Face, side))->origin.x,
               WithinAbs(35.0, 1e-9));
    CHECK(findFace(along, bossed.faceNames[static_cast<std::size_t>(side)]) >= 0);

    // A square boss: all its faces are flat, and its sides move with it.
    const NamedShape block =
        nameFaces(translated(makeBox(10.0, 10.0, 5.0), 10.0, 10.0, 10.0), "blk/f");
    const NamedShape stepped = combine(box, block, Combination::Join);
    std::vector<int> blockFaces;
    for (int i = 0; i < subShapeCount(stepped.shape, SubShapeKind::Face); ++i) {
        if (stepped.faceNames[static_cast<std::size_t>(i)].rfind("blk/", 0) == 0) {
            blockFaces.push_back(i);
        }
    }
    REQUIRE(blockFaces.size() == 5);
    const NamedShape across = moveFaces(stepped, blockFaces, {15.0, 0.0, 0.0}, "m");
    CHECK_THAT(volume(across.shape), WithinRel(volume(stepped.shape), 1e-9));
    const int east = faceWhere(across, flatFacing(1, 0, 0, 35.0));
    CHECK(boundingBox(subShape(across.shape, SubShapeKind::Face, east))->min.z > 9.99);

    CHECK_THROWS_AS(moveFaces(box, {top}, {0.0, 0.0, 0.0}, "m"), std::invalid_argument);
}

TEST_CASE("Check Geometry says what is wrong with a shape, and where", "[kernel][direct]") {
    CHECK(checkGeometry(makeBox(10.0, 10.0, 10.0)).empty());
    CHECK(checkGeometry(plate().shape).empty());

    // Five faces of a box, joined into a shell: open where the sixth would be.
    BRep_Builder builder;
    TopoDS_Shell shell;
    builder.MakeShell(shell);
    int count = 0;
    TopoDS_Shape topFace;
    for (TopExp_Explorer face(occt::unwrap(makeBox(10.0, 10.0, 10.0)), TopAbs_FACE); face.More();
         face.Next()) {
        if (++count == 6) {
            topFace = face.Current();
        } else {
            builder.Add(shell, face.Current());
        }
    }
    const auto open = checkGeometry(occt::wrap(shell));
    REQUIRE(open.size() == 1);
    CHECK_THAT(open[0].description, ContainsSubstring("surfaces, not a solid"));
    CHECK_THAT(open[0].description, ContainsSubstring("4 edges border only one face"));
    CHECK(open[0].edges.size() == 4);
    CHECK(open[0].faces.size() == 4);

    // Inside out.
    const auto inverted =
        checkGeometry(occt::wrap(occt::unwrap(makeBox(10.0, 10.0, 10.0)).Reversed()));
    REQUIRE(inverted.size() == 1);
    CHECK_THAT(inverted[0].description, ContainsSubstring("inside out"));

    // A face whose outline crosses itself: a bow tie.
    BRepBuilderAPI_MakePolygon bow(gp_Pnt(0, 0, 0), gp_Pnt(10, 10, 0), gp_Pnt(10, 0, 0),
                                   gp_Pnt(0, 10, 0), Standard_True);
    const TopoDS_Face crossed = BRepBuilderAPI_MakeFace(bow.Wire(), Standard_True).Face();
    const auto bad = checkGeometry(occt::wrap(crossed));
    bool outline = false;
    for (const GeometryProblem& problem : bad) {
        outline = outline || problem.description.find("crosses itself") != std::string::npos;
    }
    CHECK(outline);

    // Two solids that overlap, in one shape: their faces cross.
    TopoDS_Compound both;
    builder.MakeCompound(both);
    builder.Add(both, occt::unwrap(makeBox(10.0, 10.0, 10.0)));
    builder.Add(both, occt::unwrap(translated(makeBox(10.0, 10.0, 10.0), 5.0, 5.0, 5.0)));
    const auto overlapping = checkGeometry(occt::wrap(both));
    REQUIRE(overlapping.size() == 1);
    CHECK_THAT(overlapping[0].description, ContainsSubstring("cross"));
    CHECK_FALSE(overlapping[0].faces.empty());

    CHECK_THROWS_AS(checkGeometry(Shape()), std::invalid_argument);
}

TEST_CASE("Repair sews loose faces into a solid and turns it right way out", "[kernel][direct]") {
    // The six faces of a box, loose.
    BRep_Builder builder;
    TopoDS_Compound loose;
    builder.MakeCompound(loose);
    for (TopExp_Explorer face(occt::unwrap(makeBox(10.0, 20.0, 30.0)), TopAbs_FACE); face.More();
         face.Next()) {
        builder.Add(loose, face.Current());
    }
    const NamedShape faces = nameFaces(occt::wrap(loose), "imp/f");
    REQUIRE_FALSE(checkGeometry(faces.shape).empty());
    const NamedShape fixed = repaired(faces, "fix");
    CHECK(countTopology(fixed.shape).solids == 1);
    CHECK(checkGeometry(fixed.shape).empty());
    CHECK_THAT(volume(fixed.shape), WithinRel(6000.0, 1e-9));
    for (const std::string& name : faces.faceNames) {
        CHECK(findFace(fixed, name) >= 0);
    }

    const NamedShape inverted =
        nameFaces(occt::wrap(occt::unwrap(makeBox(10.0, 10.0, 10.0)).Reversed()), "imp/f");
    const NamedShape righted = repaired(inverted, "fix");
    CHECK_THAT(volume(righted.shape), WithinRel(1000.0, 1e-9));
    CHECK(checkGeometry(righted.shape).empty());

    // Five faces cannot be made into a solid.
    TopoDS_Compound five;
    builder.MakeCompound(five);
    int count = 0;
    for (TopExp_Explorer face(occt::unwrap(makeBox(10.0, 10.0, 10.0)), TopAbs_FACE); face.More();
         face.Next()) {
        if (++count < 6) {
            builder.Add(five, face.Current());
        }
    }
    CHECK_THROWS_WITH(repaired(nameFaces(occt::wrap(five), "imp/f"), "fix"),
                      ContainsSubstring("do not close up"));

    // The faces of a box with its top lifted 0.2 off the rest: a gap too wide by default, and
    // closed when the repair is told to close gaps that wide.
    TopoDS_Compound gapped;
    builder.MakeCompound(gapped);
    for (TopExp_Explorer face(occt::unwrap(makeBox(10.0, 10.0, 10.0)), TopAbs_FACE); face.More();
         face.Next()) {
        const auto plane = planeOfFace(occt::wrap(face.Current()));
        const bool top = plane.has_value() && plane->normal.z > 0.999;
        builder.Add(gapped,
                    top ? occt::unwrap(translated(occt::wrap(face.Current()), 0.0, 0.0, 0.2))
                        : face.Current());
    }
    const NamedShape apart = nameFaces(occt::wrap(gapped), "imp/f");
    CHECK_THROWS_WITH(repaired(apart, "fix"), ContainsSubstring("gaps wider than it closes"));
    const NamedShape closed = repaired(apart, "fix", 0.5);
    CHECK(countTopology(closed.shape).solids == 1);
    CHECK(isValid(closed.shape));
    CHECK_THAT(volume(closed.shape), WithinRel(1000.0, 0.03));
    CHECK_THROWS_AS(repaired(apart, "fix", 0.0), std::invalid_argument);
}
