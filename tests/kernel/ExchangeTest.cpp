// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/core/Path.h>
#include <lcad/kernel/Error.h>
#include <lcad/kernel/Exchange.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/Transform.h>
#include <lcad/kernel/occt/ShapeAccess.h>

#include <TempDir.h>

#include <BRepAlgoAPI_Cut.hxx>
#include <BRep_Builder.hxx>
#include <IGESControl_Controller.hxx>
#include <IGESControl_Writer.hxx>
#include <Interface_Static.hxx>
#include <STEPControl_Controller.hxx>
#include <STEPControl_Writer.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <zip.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <numbers>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace lcad::kernel;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double pi = std::numbers::pi;

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << in.rdbuf();
    return bytes.str();
}

// A triangle mesh read back from a file by the test itself, not by LCAD's code.
struct ReadMesh {
    std::vector<std::array<double, 3>> vertices;
    std::vector<std::array<std::size_t, 3>> triangles;
};

// The volume a closed mesh encloses, from the triangles' orientation: positive when they go
// counter-clockwise seen from outside.
double enclosedVolume(const ReadMesh& mesh) {
    double six = 0.0;
    for (const auto& t : mesh.triangles) {
        const auto& a = mesh.vertices[t[0]];
        const auto& b = mesh.vertices[t[1]];
        const auto& c = mesh.vertices[t[2]];
        six += a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) +
               a[2] * (b[0] * c[1] - b[1] * c[0]);
    }
    return six / 6.0;
}

// Whether every edge of the mesh is used by exactly two triangles, once each way round: the
// mesh is closed and its triangles all face the same way.
bool closedAndConsistent(const ReadMesh& mesh) {
    std::map<std::pair<std::size_t, std::size_t>, int> directed;
    for (const auto& t : mesh.triangles) {
        for (int i = 0; i < 3; ++i) {
            ++directed[{t[static_cast<std::size_t>(i)], t[static_cast<std::size_t>((i + 1) % 3)]}];
        }
    }
    for (const auto& [edge, count] : directed) {
        if (count != 1) {
            return false;
        }
        const auto back = directed.find({edge.second, edge.first});
        if (back == directed.end() || back->second != 1) {
            return false;
        }
    }
    return true;
}

ReadMesh fromKernel(const Mesh& mesh) {
    ReadMesh read;
    for (const Point& p : mesh.vertices) {
        read.vertices.push_back({p.x, p.y, p.z});
    }
    for (const auto& t : mesh.triangles) {
        read.triangles.push_back({t[0], t[1], t[2]});
    }
    return read;
}

// Corners of an STL file's triangles joined where they are equal, which they are where the
// writer used one vertex.
ReadMesh joined(const std::vector<std::array<double, 3>>& corners) {
    ReadMesh mesh;
    std::map<std::array<double, 3>, std::size_t> index;
    for (std::size_t i = 0; i + 2 < corners.size(); i += 3) {
        std::array<std::size_t, 3> triangle{};
        for (std::size_t k = 0; k < 3; ++k) {
            const auto [it, added] = index.try_emplace(corners[i + k], mesh.vertices.size());
            if (added) {
                mesh.vertices.push_back(corners[i + k]);
            }
            triangle[k] = it->second;
        }
        mesh.triangles.push_back(triangle);
    }
    return mesh;
}

float floatAt(const std::string& bytes, std::size_t offset) {
    std::uint32_t bits = 0;
    for (int i = 3; i >= 0; --i) {
        bits =
            (bits << 8U) | static_cast<unsigned char>(bytes[offset + static_cast<std::size_t>(i)]);
    }
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

ReadMesh readBinaryStl(const std::string& bytes, std::uint32_t& count) {
    REQUIRE(bytes.size() >= 84);
    count = 0;
    for (int i = 3; i >= 0; --i) {
        count = (count << 8U) | static_cast<unsigned char>(bytes[80 + static_cast<std::size_t>(i)]);
    }
    REQUIRE(bytes.size() == 84 + std::size_t{count} * 50);
    std::vector<std::array<double, 3>> corners;
    for (std::uint32_t t = 0; t < count; ++t) {
        const std::size_t base = 84 + std::size_t{t} * 50 + 12;
        for (std::size_t k = 0; k < 3; ++k) {
            corners.push_back({floatAt(bytes, base + k * 12), floatAt(bytes, base + k * 12 + 4),
                               floatAt(bytes, base + k * 12 + 8)});
        }
    }
    return joined(corners);
}

std::string zipEntry(const std::filesystem::path& path, const char* name) {
    int code = 0;
    zip_t* archive = zip_open(lcad::core::toUtf8(path).c_str(), ZIP_RDONLY, &code);
    REQUIRE(archive != nullptr);
    zip_stat_t stat;
    if (zip_stat(archive, name, 0, &stat) != 0) {
        zip_close(archive);
        return {};
    }
    zip_file_t* file = zip_fopen(archive, name, 0);
    REQUIRE(file != nullptr);
    std::string bytes(stat.size, '\0');
    REQUIRE(zip_fread(file, bytes.data(), stat.size) == static_cast<zip_int64_t>(stat.size));
    zip_fclose(file);
    zip_close(archive);
    return bytes;
}

// A box 60 by 40 by 10 with a hole of radius 5 through its middle.
Shape plateWithHole() {
    const Shape box = makeBox(60.0, 40.0, 10.0);
    const Shape pin = translated(makeCylinder(5.0, 12.0), 30.0, 20.0, -1.0);
    return occt::wrap(BRepAlgoAPI_Cut(occt::unwrap(box), occt::unwrap(pin)).Shape());
}

double plateVolume() {
    return 60.0 * 40.0 * 10.0 - pi * 25.0 * 10.0;
}

// Writes the faces of a shape to an IGES file as separate trimmed surfaces, the way most
// programs write IGES: there is no solid in the file to read.
void writeIgesFaces(const TopoDS_Shape& shape, const std::filesystem::path& path) {
    IGESControl_Controller::Init();
    IGESControl_Writer writer("MM", 0);
    writer.AddShape(shape);
    writer.ComputeModel();
    REQUIRE(writer.Write(lcad::core::toUtf8(path).c_str()));
}

} // namespace

TEST_CASE("STEP files are written with a part per body, and read back exactly",
          "[kernel][exchange]") {
    const lcad::test::TempDir directory;
    const auto path = directory.file("parts.step");
    const Shape plate = plateWithHole();
    const Shape pin = translated(makeCylinder(4.0, 25.0), 100.0, 0.0, 0.0);
    exportStep({{"Bracket", plate}, {"Pin", pin}}, path);

    const std::string text = readFile(path);
    CHECK(text.rfind("ISO-10303-21;", 0) == 0);
    CHECK_THAT(text, ContainsSubstring("PRODUCT('Bracket'"));
    CHECK_THAT(text, ContainsSubstring("PRODUCT('Pin'"));
    // In millimeters.
    CHECK_THAT(text, ContainsSubstring("SI_UNIT(.MILLI.,.METRE.)"));

    const ImportResult read = importStep(path);
    REQUIRE(read.bodies.size() == 2);
    CHECK(read.notes.empty());
    CHECK(read.bodies[0].name == "Bracket");
    CHECK(read.bodies[1].name == "Pin");
    CHECK(isValid(read.bodies[0].shape));
    CHECK_THAT(volume(read.bodies[0].shape), WithinRel(plateVolume(), 1e-9));
    CHECK_THAT(volume(read.bodies[1].shape), WithinRel(pi * 16.0 * 25.0, 1e-9));
    const auto box = boundingBox(read.bodies[1].shape);
    REQUIRE(box.has_value());
    CHECK_THAT(box->min.x, WithinAbs(96.0, 1e-6));
    // The faces are the exact surfaces: one cylinder, not facets.
    CHECK(countTopology(read.bodies[0].shape).faces == countTopology(plate).faces);

    // Writing again replaces the file.
    exportStep({{"Only", pin}}, path);
    CHECK(importStep(path).bodies.size() == 1);
}

TEST_CASE("A STEP file in inches is read in millimeters", "[kernel][exchange]") {
    const lcad::test::TempDir directory;
    const auto path = directory.file("inches.stp");
    {
        // A part modeled in inches, written in inches: 1 by 2 by 1/2. The settings exist once
        // the translator is set up.
        STEPControl_Controller::Init();
        Interface_Static::SetCVal("xstep.cascade.unit", "INCH");
        Interface_Static::SetCVal("write.step.unit", "INCH");
        STEPControl_Writer writer;
        const bool transferred = writer.Transfer(occt::unwrap(makeBox(1.0, 2.0, 0.5)),
                                                 STEPControl_AsIs) == IFSelect_RetDone;
        const bool written = writer.Write(lcad::core::toUtf8(path).c_str()) == IFSelect_RetDone;
        Interface_Static::SetCVal("xstep.cascade.unit", "MM");
        Interface_Static::SetCVal("write.step.unit", "MM");
        REQUIRE(transferred);
        REQUIRE(written);
    }
    const std::string text = readFile(path);
    CHECK_THAT(text, ContainsSubstring("CONVERSION_BASED_UNIT('INCH'"));
    CHECK_THAT(text, ContainsSubstring("(0.,0.,0.5)"));
    const ImportResult read = importStep(path);
    REQUIRE(read.bodies.size() == 1);
    // Named after the file, since the part in it has no name of its own.
    CHECK(read.bodies[0].name == "inches");
    const auto box = boundingBox(read.bodies[0].shape);
    REQUIRE(box.has_value());
    CHECK_THAT(box->max.x - box->min.x, WithinAbs(25.4, 1e-6));
    CHECK_THAT(box->max.y - box->min.y, WithinAbs(50.8, 1e-6));
    CHECK_THAT(box->max.z - box->min.z, WithinAbs(12.7, 1e-6));
}

TEST_CASE("IGES surfaces are sewn into a solid where they close up", "[kernel][exchange]") {
    const lcad::test::TempDir directory;
    const auto path = directory.file("plate.igs");
    writeIgesFaces(occt::unwrap(plateWithHole()), path);
    CHECK(readFile(path).find("S      1") != std::string::npos);

    const ImportResult read = importIges(path);
    REQUIRE(read.bodies.size() == 1);
    CHECK(read.bodies[0].name == "plate");
    CHECK(countTopology(read.bodies[0].shape).solids == 1);
    CHECK(isValid(read.bodies[0].shape));
    CHECK_THAT(volume(read.bodies[0].shape), WithinRel(plateVolume(), 1e-6));
    REQUIRE(read.notes.size() == 1);
    CHECK_THAT(read.notes[0], ContainsSubstring("sewn into 1 closed solid"));
}

TEST_CASE("Surfaces that do not close up are kept as surfaces, and said so", "[kernel][exchange]") {
    const lcad::test::TempDir directory;
    const auto path = directory.file("open.igs");
    // A box without its top: five faces.
    BRep_Builder builder;
    TopoDS_Compound five;
    builder.MakeCompound(five);
    int faces = 0;
    for (TopExp_Explorer face(occt::unwrap(makeBox(10.0, 10.0, 10.0)), TopAbs_FACE); face.More();
         face.Next()) {
        if (++faces != 6) {
            builder.Add(five, face.Current());
        }
    }
    writeIgesFaces(five, path);
    const ImportResult read = importIges(path);
    REQUIRE(read.bodies.size() == 1);
    CHECK(countTopology(read.bodies[0].shape).solids == 0);
    CHECK(countTopology(read.bodies[0].shape).faces == 5);
    REQUIRE(read.notes.size() == 1);
    CHECK_THAT(read.notes[0], ContainsSubstring("5 faces that do not close up"));
    CHECK_THAT(read.notes[0], ContainsSubstring("4 edges border only one face"));
}

TEST_CASE("Files that cannot be read say why", "[kernel][exchange]") {
    const lcad::test::TempDir directory;
    CHECK_THROWS_WITH(importStep(directory.file("missing.step")),
                      ContainsSubstring("the file does not exist"));
    const auto notStep = directory.file("notes.step");
    std::ofstream(notStep) << "These are my notes, not a model.\n";
    CHECK_THROWS_AS(importStep(notStep), Error);
    CHECK_THROWS_WITH(importStep(notStep), ContainsSubstring("not a STEP file"));
    CHECK_THROWS_WITH(importIges(notStep), ContainsSubstring("IGES"));
    CHECK_THROWS_WITH(importStep(directory.path()), ContainsSubstring("folder"));
    CHECK_THROWS_WITH(exportStep({}, directory.file("empty.step")),
                      ContainsSubstring("nothing to export"));
    CHECK_THROWS_WITH(exportStep({{"Box", makeBox(1.0, 1.0, 1.0)}},
                                 directory.file("no-such-folder") / "box.step"),
                      ContainsSubstring("does not exist"));
}

TEST_CASE("A mesh is closed, faces outward, and comes closer with finer settings",
          "[kernel][exchange]") {
    // Flat faces are cut exactly.
    const Mesh box = triangulate(makeBox(60.0, 40.0, 10.0), {});
    CHECK(box.vertices.size() == 8);
    CHECK(box.triangles.size() == 12);
    CHECK(closedAndConsistent(fromKernel(box)));
    CHECK_THAT(enclosedVolume(fromKernel(box)), WithinRel(24000.0, 1e-12));

    // A curved one is approached: the triangles lie within the deviation of it.
    const Shape cylinder = makeCylinder(10.0, 20.0);
    const Mesh coarse = triangulate(cylinder, {1.0, 30.0});
    const Mesh fine = triangulate(cylinder, {0.01, 5.0});
    CHECK(closedAndConsistent(fromKernel(coarse)));
    CHECK(closedAndConsistent(fromKernel(fine)));
    CHECK(fine.triangles.size() > 4 * coarse.triangles.size());
    const double exact = pi * 100.0 * 20.0;
    const double coarseVolume = enclosedVolume(fromKernel(coarse));
    const double fineVolume = enclosedVolume(fromKernel(fine));
    // Inscribed: a little less than the cylinder, and less so when finer.
    CHECK(coarseVolume < fineVolume);
    CHECK(fineVolume < exact);
    CHECK_THAT(fineVolume, WithinRel(exact, 2e-3));
    // Every corner lies on the surface.
    for (const Point& p : fine.vertices) {
        const double r = std::hypot(p.x, p.y);
        CHECK((std::abs(r - 10.0) < 1e-6 || std::abs(p.z) < 1e-9 || std::abs(p.z - 20.0) < 1e-9));
    }
    // A shape with a hole through it.
    CHECK(closedAndConsistent(fromKernel(triangulate(plateWithHole(), {}))));

    CHECK_THROWS_AS(triangulate(Shape(), {}), std::invalid_argument);
    CHECK_THROWS_AS(triangulate(cylinder, {0.0, 10.0}), std::invalid_argument);
    CHECK_THROWS_AS(triangulate(cylinder, {0.1, 120.0}), std::invalid_argument);
}

TEST_CASE("STL files hold every body's triangles, binary or as text", "[kernel][exchange]") {
    const lcad::test::TempDir directory;
    const Shape plate = plateWithHole();
    const Shape pin = translated(makeBox(5.0, 5.0, 5.0), 100.0, 0.0, 0.0);
    const MeshSettings settings{0.01, 10.0};
    const std::size_t plateTriangles = triangulate(plate, settings).triangles.size();

    const auto binary = directory.file("parts.stl");
    exportStl({{"Bracket", plate}, {"Pin", pin}}, binary, settings);
    const std::string bytes = readFile(binary);
    CHECK(bytes.rfind("Written by LCAD", 0) == 0);
    std::uint32_t count = 0;
    const ReadMesh read = readBinaryStl(bytes, count);
    CHECK(count == plateTriangles + 12);
    CHECK(closedAndConsistent(read));
    // Single precision.
    CHECK_THAT(enclosedVolume(read), WithinRel(plateVolume() + 125.0, 2e-3));
    // The normals face out: the first triangle's normal agrees with its corners.
    {
        const float nx = floatAt(bytes, 84);
        const float ny = floatAt(bytes, 88);
        const float nz = floatAt(bytes, 92);
        CHECK_THAT(std::sqrt(nx * nx + ny * ny + nz * nz), WithinAbs(1.0, 1e-5));
    }

    const auto text = directory.file("pin.stl");
    exportStl({{"Pin", pin}}, text, settings, StlEncoding::Text);
    const std::string written = readFile(text);
    CHECK(written.rfind("solid Pin\n", 0) == 0);
    CHECK_THAT(written, ContainsSubstring("endsolid Pin"));
    const std::regex vertex(R"(vertex (\S+) (\S+) (\S+))");
    std::vector<std::array<double, 3>> corners;
    for (auto it = std::sregex_iterator(written.begin(), written.end(), vertex);
         it != std::sregex_iterator(); ++it) {
        corners.push_back({std::stod((*it)[1]), std::stod((*it)[2]), std::stod((*it)[3])});
    }
    CHECK(corners.size() == 36);
    const ReadMesh fromText = joined(corners);
    CHECK(closedAndConsistent(fromText));
    CHECK_THAT(enclosedVolume(fromText), WithinRel(125.0, 1e-12));
}

TEST_CASE("OBJ files hold each body as a named object", "[kernel][exchange]") {
    const lcad::test::TempDir directory;
    const auto path = directory.file("parts.obj");
    exportObj({{"Bracket", plateWithHole()}, {"Pin", makeBox(5.0, 5.0, 5.0)}}, path, {0.01, 10.0});
    std::istringstream lines(readFile(path));
    std::vector<std::string> objects;
    std::vector<ReadMesh> meshes;
    std::vector<std::array<double, 3>> all;
    std::size_t firstOfObject = 0;
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream words(line);
        std::string kind;
        words >> kind;
        if (kind == "o") {
            objects.push_back(line.substr(2));
            meshes.emplace_back();
            firstOfObject = all.size();
        } else if (kind == "v") {
            std::array<double, 3> p{};
            words >> p[0] >> p[1] >> p[2];
            all.push_back(p);
            meshes.back().vertices.push_back(p);
        } else if (kind == "f") {
            std::array<std::size_t, 3> t{};
            words >> t[0] >> t[1] >> t[2];
            for (std::size_t& index : t) {
                // OBJ counts from 1, through the whole file.
                index -= 1 + firstOfObject;
            }
            meshes.back().triangles.push_back(t);
        }
    }
    CHECK(objects == std::vector<std::string>{"Bracket", "Pin"});
    REQUIRE(meshes.size() == 2);
    CHECK(closedAndConsistent(meshes[0]));
    CHECK_THAT(enclosedVolume(meshes[0]), WithinRel(plateVolume(), 2e-3));
    CHECK_THAT(enclosedVolume(meshes[1]), WithinRel(125.0, 1e-12));
}

TEST_CASE("3MF files are a package with a millimeter model of named objects",
          "[kernel][exchange]") {
    const lcad::test::TempDir directory;
    const auto path = directory.file("parts.3mf");
    export3mf({{"Bracket & Co <1>", plateWithHole()}, {"Pin", makeBox(5.0, 5.0, 5.0)}}, path,
              {0.01, 10.0});

    const std::string types = zipEntry(path, "[Content_Types].xml");
    CHECK_THAT(types, ContainsSubstring("application/vnd.ms-package.3dmanufacturing-3dmodel+xml"));
    const std::string relations = zipEntry(path, "_rels/.rels");
    CHECK_THAT(relations, ContainsSubstring("Target=\"/3D/3dmodel.model\""));
    CHECK_THAT(relations,
               ContainsSubstring("http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"));
    const std::string model = zipEntry(path, "3D/3dmodel.model");
    CHECK_THAT(model, ContainsSubstring("unit=\"millimeter\""));
    CHECK_THAT(model, ContainsSubstring(
                          "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\""));
    // The name is escaped for XML.
    CHECK_THAT(model, ContainsSubstring("name=\"Bracket &amp; Co &lt;1&gt;\""));
    CHECK_THAT(model, ContainsSubstring("<item objectid=\"2\"/>"));

    // Each object's mesh, read with patterns of the test's own.
    const std::regex vertex(R"re(<vertex x="([^"]+)" y="([^"]+)" z="([^"]+)"/>)re");
    const std::regex triangle(R"re(<triangle v1="(\d+)" v2="(\d+)" v3="(\d+)"/>)re");
    std::vector<ReadMesh> meshes;
    // Objects by plain search: a pattern spanning a whole object makes std::regex recurse once
    // per character, which overflows the stack on a large mesh.
    for (std::size_t from = model.find("<object "); from != std::string::npos;
         from = model.find("<object ", from + 1)) {
        const std::size_t to = model.find("</object>", from);
        REQUIRE(to != std::string::npos);
        const std::string body = model.substr(from, to - from);
        ReadMesh mesh;
        for (auto v = std::sregex_iterator(body.begin(), body.end(), vertex);
             v != std::sregex_iterator(); ++v) {
            mesh.vertices.push_back({std::stod((*v)[1]), std::stod((*v)[2]), std::stod((*v)[3])});
        }
        for (auto t = std::sregex_iterator(body.begin(), body.end(), triangle);
             t != std::sregex_iterator(); ++t) {
            mesh.triangles.push_back(
                {std::stoul((*t)[1]), std::stoul((*t)[2]), std::stoul((*t)[3])});
        }
        meshes.push_back(mesh);
    }
    REQUIRE(meshes.size() == 2);
    for (const ReadMesh& mesh : meshes) {
        CHECK(closedAndConsistent(mesh));
        for (const auto& t : mesh.triangles) {
            CHECK(t[0] < mesh.vertices.size());
            CHECK((t[0] != t[1] && t[1] != t[2] && t[0] != t[2]));
        }
    }
    CHECK_THAT(enclosedVolume(meshes[0]), WithinRel(plateVolume(), 2e-3));
    CHECK_THAT(enclosedVolume(meshes[1]), WithinRel(125.0, 1e-12));
}
