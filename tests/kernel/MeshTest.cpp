// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Check.h>
#include <lcad/kernel/Error.h>
#include <lcad/kernel/Exchange.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/MeshSolid.h>
#include <lcad/kernel/MeshSurfaces.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/kernel/Transform.h>

#include <TempDir.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <fstream>
#include <map>

using namespace lcad::kernel;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

const double pi = 3.14159265358979323846;

int countOf(const MeshSurfaces& found, SurfaceKind kind) {
    return static_cast<int>(std::count_if(found.surfaces.begin(), found.surfaces.end(),
                                          [kind](const MeshSurface& s) {
                                              return !s.facet && s.kind == kind;
                                          }));
}

const MeshSurface& onlyOf(const MeshSurfaces& found, SurfaceKind kind) {
    return *std::find_if(found.surfaces.begin(), found.surfaces.end(), [kind](const MeshSurface& s) {
        return !s.facet && s.kind == kind;
    });
}

bool isClosed(const Mesh& mesh) {
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> sides;
    for (const auto& c : mesh.triangles) {
        for (std::size_t k = 0; k < 3; ++k) {
            ++sides[{c[k], c[(k + 1) % 3]}];
        }
    }
    return std::all_of(sides.begin(), sides.end(), [&sides](const auto& side) {
        const auto twin = sides.find({side.first.second, side.first.first});
        return twin != sides.end() && twin->second == side.second;
    });
}

// A plate with a round hole and a rounded corner.
Shape bracket() {
    const NamedShape plate = nameFaces(makeBox(60.0, 40.0, 10.0), "plate");
    const NamedShape pin = nameFaces(translated(makeCylinder(5.0, 12.0), 30.0, 20.0, -1.0), "pin");
    return combine(plate, pin, Combination::Cut).shape;
}

} // namespace

TEST_CASE("A mesh comes back from each kind of file the same", "[mesh]") {
    lcad::test::TempDir directory;
    const Shape box = makeBox(30.0, 20.0, 10.0);
    const MeshSettings settings{0.05, 15.0};
    const std::filesystem::path binary = directory.path() / "box.stl";
    const std::filesystem::path text = directory.path() / "text.stl";
    const std::filesystem::path obj = directory.path() / "box.obj";
    const std::filesystem::path threeMf = directory.path() / "box.3mf";
    exportStl({{"Box", box}}, binary, settings, StlEncoding::Binary);
    exportStl({{"Box", box}}, text, settings, StlEncoding::Text);
    exportObj({{"Box", box}}, obj, settings);
    export3mf({{"Box", box}}, threeMf, settings);
    for (const std::filesystem::path& path : {binary, text, obj, threeMf}) {
        INFO(path);
        const Mesh mesh = readMesh(path);
        CHECK(mesh.vertices.size() == 8);
        CHECK(mesh.triangles.size() == 12);
        CHECK(isClosed(mesh));
        CHECK_THAT(meshVolume(mesh), WithinRel(6000.0, 1e-6));
    }
}

TEST_CASE("A file without triangles is refused", "[mesh]") {
    lcad::test::TempDir directory;
    const std::filesystem::path path = directory.path() / "notes.stl";
    std::ofstream(path) << "Not a mesh.\n";
    CHECK_THROWS_AS(readMesh(path), Error);
    CHECK_THROWS_AS(readMesh(directory.path() / "missing.obj"), Error);
}

TEST_CASE("The surfaces of a mesh are found for what they are", "[mesh]") {
    const Mesh box = triangulate(makeBox(30.0, 20.0, 10.0), {0.05, 15.0});
    const MeshSurfaces plain = findSurfaces(box);
    CHECK(plain.surfaces.size() == 6);
    CHECK(countOf(plain, SurfaceKind::Plane) == 6);
    CHECK_THAT(plain.share, WithinAbs(1.0, 1e-9));

    const MeshSurfaces drilled = findSurfaces(triangulate(bracket(), {0.01, 10.0}));
    CHECK(countOf(drilled, SurfaceKind::Plane) == 6);
    REQUIRE(countOf(drilled, SurfaceKind::Cylinder) == 1);
    const MeshSurface& hole = onlyOf(drilled, SurfaceKind::Cylinder);
    CHECK_THAT(hole.radius, WithinAbs(5.0, 1e-3));
    CHECK_FALSE(hole.outward);

    const MeshSurfaces ball = findSurfaces(triangulate(makeSphere(12.0), {0.01, 8.0}));
    REQUIRE(countOf(ball, SurfaceKind::Sphere) == 1);
    CHECK_THAT(onlyOf(ball, SurfaceKind::Sphere).radius, WithinAbs(12.0, 5e-3));

    const MeshSurfaces ring = findSurfaces(triangulate(makeTorus(20.0, 5.0), {0.01, 8.0}));
    REQUIRE(countOf(ring, SurfaceKind::Torus) == 1);
    CHECK_THAT(onlyOf(ring, SurfaceKind::Torus).radius, WithinAbs(20.0, 1e-2));
    CHECK_THAT(onlyOf(ring, SurfaceKind::Torus).tubeRadius, WithinAbs(5.0, 1e-2));
}

TEST_CASE("A solid of exact faces is made from a mesh", "[mesh]") {
    const Shape part = bracket();
    const double wanted = volume(part);
    const Mesh mesh = triangulate(part, {0.01, 10.0});
    const MeshSolids made = solidsFromMesh(mesh, findSurfaces(mesh));
    REQUIRE(made.solids.size() == 1);
    CHECK(made.facetedParts == 0);
    CHECK(made.openParts == 0);
    CHECK(isValid(made.solids.front()));
    CHECK_THAT(volume(made.solids.front()), WithinRel(wanted, 1e-5));
    // The wall of the hole is one surface, cut in two where it would otherwise meet itself.
    CHECK(countTopology(made.solids.front()).faces == 8);
}

TEST_CASE("A mesh that does not close makes no solid", "[mesh]") {
    Mesh mesh = triangulate(makeBox(10.0, 10.0, 10.0), {0.05, 15.0});
    mesh.triangles.pop_back();
    const MeshSolids made = solidsFromMesh(mesh, findSurfaces(mesh));
    CHECK(made.solids.empty());
    CHECK(made.openParts == 1);
}

TEST_CASE("The edge nearest a point is found, and not the seam beside it", "[mesh]") {
    const Shape cylinder = makeCylinder(10.0, 20.0);
    // A place where the top rim and the seam of the wall meet.
    const int rim = nearestEdge(cylinder, {10.0, 0.0, 20.0}, 0.01);
    REQUIRE(rim >= 0);
    CHECK_THAT(length(subShape(cylinder, SubShapeKind::Edge, rim)), WithinRel(2.0 * pi * 10.0, 1e-9));
    CHECK(nearestEdge(cylinder, {50.0, 0.0, 20.0}, 1.0) == -1);
}

TEST_CASE("How far points lie from the faces of a solid is measured from the faces", "[mesh]") {
    const Shape box = makeBox(10.0, 10.0, 10.0);
    CHECK_THAT(farthestFromFaces(box, {{5.0, 5.0, 10.0}, {0.0, 0.0, 0.0}}), WithinAbs(0.0, 1e-9));
    // In the middle of the solid: 5 from every face.
    CHECK_THAT(farthestFromFaces(box, {{5.0, 5.0, 5.0}, {5.0, 5.0, 9.0}}), WithinAbs(5.0, 1e-9));
}
