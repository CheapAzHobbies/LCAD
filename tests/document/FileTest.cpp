// SPDX-License-Identifier: GPL-3.0-or-later

#include "Archive.h"
#include "ReferenceModel.h"
#include "ReferenceModelChecks.h"

#include <lcad/core/Log.h>
#include <lcad/core/Version.h>
#include <lcad/document/File.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/Plane.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/Serialization.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/kernel/occt/ShapeAccess.h>
#include <lcad/sketch/Edit.h>

#include <TempDir.h>

#include <BRep_Builder.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>

#if !defined(_WIN32)
#include <unistd.h>
#endif

using namespace lcad::document;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinRel;
using lcad::kernel::makeBox;
using lcad::kernel::makeSphere;
using lcad::test::TempDir;
using nlohmann::json;

namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << stream.rdbuf();
    return bytes.str();
}

void writeFile(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << bytes;
}

std::vector<std::string> filesIn(const std::filesystem::path& directory) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

// A copy of the valid document at `source`, written to `target` after `change` has edited its
// parsed manifest and model and its raw entries.
void writeVariant(
    const std::filesystem::path& source, const std::filesystem::path& target,
    const std::function<void(json& manifest, json& model, detail::Entries& entries)>& change) {
    detail::Entries entries = detail::readArchive(source);
    json manifest = json::parse(entries.at("manifest.json"));
    json model = json::parse(entries.at("document.json"));
    entries.erase("manifest.json");
    entries.erase("document.json");
    change(manifest, model, entries);
    std::vector<std::pair<std::string, std::string>> out;
    if (!manifest.is_null()) {
        out.emplace_back("manifest.json",
                         manifest.is_string() ? manifest.get<std::string>() : manifest.dump());
    }
    if (!model.is_null()) {
        out.emplace_back("document.json",
                         model.is_string() ? model.get<std::string>() : model.dump());
    }
    for (const auto& entry : entries) {
        out.emplace_back(entry);
    }
    detail::writeArchive(target, out);
}

std::string errorFrom(const std::filesystem::path& path) {
    try {
        (void)loadDocument(path);
    } catch (const FileError& error) {
        return error.what();
    }
    return {};
}

// A box with one side missing, presented as a solid. It can be written and read, but it does
// not enclose a volume and fails the kernel's validity check.
lcad::kernel::Shape openBox() {
    const lcad::kernel::Shape box = makeBox(1.0, 2.0, 3.0);
    BRep_Builder builder;
    TopoDS_Shell shell;
    builder.MakeShell(shell);
    for (int i = 0; i < 5; ++i) {
        builder.Add(shell, lcad::kernel::occt::unwrap(
                               lcad::kernel::subShape(box, lcad::kernel::SubShapeKind::Face, i)));
    }
    TopoDS_Solid solid;
    builder.MakeSolid(solid);
    builder.Add(solid, shell);
    return lcad::kernel::occt::wrap(solid);
}

} // namespace

// The acceptance test of Phase 1 at the level of the document: create, add solids, save,
// reopen, and find the same geometry.
TEST_CASE("A saved document reopens with the same bodies and exact geometry", "[document][file]") {
    const TempDir directory;
    const std::filesystem::path path = directory.file("reference.lcad");
    const auto original = lcad::test::buildReferenceDocument();

    saveDocument(*original, path);
    const LoadedDocument loaded = loadDocument(path);

    REQUIRE(loaded.document != nullptr);
    lcad::test::checkReferenceDocument(*loaded.document);
    CHECK(loaded.formatVersion == currentFormatVersion);
    CHECK(loaded.writtenBy == "LCAD " + lcad::core::version());
    CHECK(loaded.warnings.empty());

    SECTION("a reopened document has no unsaved changes and no undo history") {
        CHECK_FALSE(loaded.document->isModified());
        CHECK_FALSE(loaded.document->canUndo());
        CHECK_FALSE(loaded.document->canRedo());
    }

    SECTION("saving what was loaded gives the same document again") {
        const std::filesystem::path second = directory.file("second.lcad");
        saveDocument(*loaded.document, second);
        lcad::test::checkReferenceDocument(*loadDocument(second).document);
    }

    SECTION("saving does not change the document or mark it saved") {
        CHECK(original->isModified());
        CHECK(original->canUndo());
    }
}

namespace {

// A document with a body and two sketches: a fully constrained 40 by 25 rectangle on the XZ
// plane with a construction diagonal, and a loose circle and arc on a plane away from the
// origin. Returns the ids of the width and height dimensions.
struct SketchedDocument {
    std::unique_ptr<Document> document;
    ObjectId outline;
    ObjectId loose;
    lcad::sketch::ConstraintId width = 0;
    lcad::sketch::ConstraintId height = 0;
};

SketchedDocument buildSketchedDocument() {
    using lcad::sketch::ConstraintKind;
    SketchedDocument made;
    made.document = std::make_unique<Document>();
    made.document->addBody("Box", makeBox(10.0, 20.0, 30.0));

    made.outline = made.document->addSketch("Outline", lcad::kernel::planeXZ());
    lcad::sketch::Sketch s;
    const auto bottom = s.addLine({0.0, 0.0}, {38.0, 1.0});
    const auto right = s.addLine({38.0, 1.0}, {39.0, 24.0});
    const auto top = s.addLine({39.0, 24.0}, {1.0, 25.0});
    const auto left = s.addLine({1.0, 25.0}, {0.0, 0.0});
    const auto join = [&s](lcad::sketch::EntityId a, lcad::sketch::EntityId b) {
        s.addConstraint(ConstraintKind::Coincident, s.find(a)->end, s.find(b)->start);
    };
    join(bottom, right);
    join(right, top);
    join(top, left);
    join(left, bottom);
    s.addConstraint(ConstraintKind::Horizontal, bottom);
    s.addConstraint(ConstraintKind::Horizontal, top);
    s.addConstraint(ConstraintKind::Vertical, right);
    s.addConstraint(ConstraintKind::Vertical, left);
    s.addConstraint(ConstraintKind::Coincident, s.find(bottom)->start, s.origin());
    made.width = s.addConstraint(ConstraintKind::Distance, s.find(bottom)->start,
                                 s.find(bottom)->end, 0, 40.0);
    made.height = s.addConstraint(ConstraintKind::Distance, s.find(right)->start,
                                  s.find(right)->end, 0, 25.0);
    // A construction line from corner to corner. It is tied to the corners, so it adds no freedom.
    const auto diagonal = s.addLine({0.0, 0.0}, {40.0, 25.0});
    s.setConstruction(diagonal, true);
    s.addConstraint(ConstraintKind::Coincident, s.find(diagonal)->start, s.find(bottom)->start);
    s.addConstraint(ConstraintKind::Coincident, s.find(diagonal)->end, s.find(right)->end);
    REQUIRE(s.solve().isFullyConstrained());
    made.document->setSketchGeometry(made.outline, s, "Rectangle");

    made.loose = made.document->addSketch(
        "Löcher", lcad::kernel::makePlane({5.0, 6.0, 7.0}, {1.0, 0.0, 1.0}, {0.0, 1.0, 0.0}));
    lcad::sketch::Sketch t;
    const auto circle = t.addCircle({12.5, -3.0}, 4.25);
    t.addArc({0.0, 0.0}, {6.0, 0.0}, {0.0, 6.0});
    t.addConstraint(ConstraintKind::Diameter, circle, 0, 0, 8.5);
    made.document->setSketchGeometry(made.loose, t, "Circle");
    made.document->setSketchVisible(made.loose, false);
    return made;
}

} // namespace

TEST_CASE("Sketches reopen with their plane, geometry, and constraints", "[document][file]") {
    const TempDir directory;
    const std::filesystem::path path = directory.file("sketched.lcad");
    const SketchedDocument made = buildSketchedDocument();

    saveDocument(*made.document, path);
    const LoadedDocument loaded = loadDocument(path);
    Document& document = *loaded.document;

    CHECK(loaded.warnings.empty());
    REQUIRE(document.sketches().size() == 2);
    CHECK(document.bodies().size() == 1);
    CHECK(document.nextObjectId() == made.document->nextObjectId());

    const SketchItem* outline = document.findSketch(made.outline);
    REQUIRE(outline != nullptr);
    CHECK(outline->name == "Outline");
    CHECK(outline->visible);
    CHECK(outline->plane.normal.y == -1.0);
    const lcad::sketch::Sketch& original = made.document->findSketch(made.outline)->geometry;
    const lcad::sketch::Sketch& copy = outline->geometry;
    REQUIRE(copy.entities().size() == original.entities().size());
    for (std::size_t i = 0; i < copy.entities().size(); ++i) {
        const lcad::sketch::Entity& a = original.entities()[i];
        const lcad::sketch::Entity& b = copy.entities()[i];
        CAPTURE(a.id);
        CHECK(b.id == a.id);
        CHECK(b.kind == a.kind);
        CHECK(b.construction == a.construction);
        CHECK(b.fixed == a.fixed);
        CHECK(b.owner == a.owner);
        // Numbers are written with every digit, so they come back as they went.
        CHECK(b.x == a.x);
        CHECK(b.y == a.y);
        CHECK(b.start == a.start);
        CHECK(b.end == a.end);
    }
    REQUIRE(copy.constraints().size() == original.constraints().size());
    for (std::size_t i = 0; i < copy.constraints().size(); ++i) {
        CHECK(copy.constraints()[i].id == original.constraints()[i].id);
        CHECK(copy.constraints()[i].kind == original.constraints()[i].kind);
        CHECK(copy.constraints()[i].first == original.constraints()[i].first);
        CHECK(copy.constraints()[i].value == original.constraints()[i].value);
    }

    // It is still the same working sketch: fully constrained, and it follows its dimensions.
    CHECK(copy.state().isFullyConstrained());
    REQUIRE(copy.profiles().size() == 1);
    CHECK(std::abs(copy.profiles()[0].area - 1000.0) < 1e-6);
    lcad::sketch::Sketch edited = copy;
    edited.setConstraintValue(made.width, 60.0);
    CHECK(edited.solve().isFullyConstrained());
    CHECK(std::abs(edited.profiles()[0].area - 1500.0) < 1e-6);
    // New geometry gets ids that were never used.
    CHECK(edited.addPoint({1.0, 1.0}) == original.nextEntityId());

    const SketchItem* loose = document.findSketch(made.loose);
    REQUIRE(loose != nullptr);
    CHECK(loose->name == "Löcher");
    CHECK_FALSE(loose->visible);
    CHECK(std::abs(loose->plane.normal.x - std::sqrt(0.5)) < 1e-12);
    CHECK(loose->plane.origin.z == 7.0);
    CHECK(loose->geometry.state().degreesOfFreedom ==
          made.document->findSketch(made.loose)->geometry.state().degreesOfFreedom);

    SECTION("in the file, a sketch is readable JSON") {
        const json model = json::parse(detail::readArchive(path).at("document.json"));
        REQUIRE(model["sketches"].size() == 2);
        CHECK(model["sketches"][0]["name"] == "Outline");
        CHECK(model["sketches"][0]["plane"]["normal"] == json::array({0.0, -1.0, 0.0}));
        CHECK(model["sketches"][0]["entities"][0]["kind"] == "point");
        CHECK(model["sketches"][0]["constraints"][0]["kind"] == "Coincident");
        CHECK(model["sketches"][1]["constraints"][0]["kind"] == "Diameter");
        CHECK(model["sketches"][1]["constraints"][0]["value"] == 8.5);
    }
}

TEST_CASE("A document with a damaged sketch is refused", "[document][file]") {
    using Change = std::function<void(json&)>;
    struct Damage {
        const char* what;
        Change change; // applied to the first sketch
        const char* expectedInMessage;
    };
    const Damage damage = GENERATE(
        Damage{"no name", [](json& s) { s.erase("name"); }, "has no \"name\""},
        Damage{"a plane without a normal", [](json& s) { s["plane"]["normal"] = {0, 0, 0}; },
               "is not a plane"},
        Damage{"a plane with two numbers", [](json& s) { s["plane"]["origin"] = {0, 0}; },
               "is not a list of 3 numbers"},
        Damage{"an entity of an unknown kind",
               [](json& s) { s["entities"][1]["kind"] = "squiggle"; }, "unknown kind \"squiggle\""},
        Damage{"a point without a coordinate", [](json& s) { s["entities"][0].erase("y"); },
               "has no \"y\""},
        Damage{"a line whose end is not there",
               [](json& s) {
                   for (json& e : s["entities"]) {
                       if (e["kind"] == "line") {
                           e["end"] = 9999;
                           break;
                       }
                   }
               },
               "does not hold together"},
        Damage{"a constraint of an unknown kind",
               [](json& s) { s["constraints"][0]["kind"] = "Sideways"; },
               "unknown kind \"Sideways\""},
        Damage{"a constraint on something that is not there",
               [](json& s) { s["constraints"][0]["entities"] = {9998, 9999, 0}; },
               "does not hold together"},
        Damage{"a constraint without a value", [](json& s) { s["constraints"][0].erase("value"); },
               "has no \"value\""},
        Damage{"no list of constraints", [](json& s) { s["constraints"] = "none"; },
               "is not a list"});
    CAPTURE(damage.what);
    const TempDir directory;
    saveDocument(*buildSketchedDocument().document, directory.file("good.lcad"));
    writeVariant(
        directory.file("good.lcad"), directory.file("bad.lcad"),
        [&damage](json&, json& model, detail::Entries&) { damage.change(model["sketches"][0]); });

    const std::string error = errorFrom(directory.file("bad.lcad"));

    CHECK_THAT(error, ContainsSubstring(damage.expectedInMessage));
    CHECK_THAT(error, ContainsSubstring("bad.lcad"));
}

TEST_CASE("An empty document can be saved and reopened", "[document][file]") {
    const TempDir directory;
    const std::filesystem::path path = directory.file("empty.lcad");

    saveDocument(Document(), path);
    const LoadedDocument loaded = loadDocument(path);

    CHECK(loaded.document->bodies().empty());
    CHECK(loaded.document->nextObjectId() == 1);
}

TEST_CASE("The file is a zip of readable JSON and one geometry file per body", "[document][file]") {
    const TempDir directory;
    const std::filesystem::path path = directory.file("layout.lcad");
    saveDocument(*lcad::test::buildReferenceDocument(), path);

    const detail::Entries entries = detail::readArchive(path);

    std::vector<std::string> names;
    for (const auto& entry : entries) {
        names.push_back(entry.first);
    }
    CHECK(names == std::vector<std::string>{"document.json", "geometry/1.brep", "geometry/2.brep",
                                            "geometry/4.brep", "geometry/5.brep", "geometry/6.brep",
                                            "manifest.json"});
    const json manifest = json::parse(entries.at("manifest.json"));
    CHECK(manifest["format"] == "lcad");
    CHECK(manifest["formatVersion"] == 1);
    CHECK(manifest["writtenBy"]["application"] == "LCAD");
    CHECK(manifest["writtenBy"]["version"] == lcad::core::version());
    CHECK(manifest["units"]["length"] == "mm");
    const json model = json::parse(entries.at("document.json"));
    CHECK(model["nextId"] == 7);
    REQUIRE(model["bodies"].size() == 5);
    CHECK(model["bodies"][2]["name"] == "Kugel Ø14");
    CHECK(model["bodies"][2]["visible"] == false);
    CHECK(model["bodies"][2]["geometry"]["file"] == "geometry/4.brep");
    // The geometry files are the kernel's own encoding, usable on their own.
    CHECK_FALSE(lcad::kernel::fromBrep(entries.at("geometry/1.brep")).isNull());
}

TEST_CASE("Saving replaces the file in one step and never writes into it", "[document][file]") {
    const TempDir directory;
    const std::filesystem::path path = directory.file("part.lcad");
    Document document;
    document.addBody("First", makeBox(1.0, 2.0, 3.0));
    saveDocument(document, path);
    // A second name for the same file. If saving wrote into the existing file, the new content
    // would show up here too.
    const std::filesystem::path oldVersion = directory.file("old-version.lcad");
    std::filesystem::create_hard_link(path, oldVersion);

    document.addBody("Second", makeSphere(2.0));
    saveDocument(document, path);

    CHECK(loadDocument(path).document->bodies().size() == 2);
    CHECK(loadDocument(oldVersion).document->bodies().size() == 1);
    // And no temporary file is left behind.
    CHECK(filesIn(directory.path()) == std::vector<std::string>{"old-version.lcad", "part.lcad"});
}

// Windows keeps no Unix permissions: a file there is read-only or not.
#if !defined(_WIN32)
TEST_CASE("Saving keeps the permissions of the file it replaces", "[document][file]") {
    const TempDir directory;
    const std::filesystem::path path = directory.file("private.lcad");
    Document document;
    document.addBody("Box", makeBox(1.0, 2.0, 3.0));
    saveDocument(document, path);
    const auto ownerOnly = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
    std::filesystem::permissions(path, ownerOnly);

    document.addBody("Ball", makeSphere(1.0));
    saveDocument(document, path);

    CHECK(std::filesystem::status(path).permissions() == ownerOnly);
    CHECK(loadDocument(path).document->bodies().size() == 2);
}
#endif

TEST_CASE("Saving through a symbolic link updates the file it points to", "[document][file]") {
    const TempDir directory;
    std::filesystem::create_directory(directory.file("real"));
    const std::filesystem::path real = directory.file("real") / "part.lcad";
    const std::filesystem::path link = directory.file("shortcut.lcad");
    Document document;
    document.addBody("Box", makeBox(1.0, 2.0, 3.0));
    saveDocument(document, real);
    std::error_code ec;
    std::filesystem::create_symlink(real, link, ec);
    if (ec) {
        // Windows lets only administrators and developer mode make symbolic links.
        WARN("not checked: this system did not let a symbolic link be made (" + ec.message() + ")");
        return;
    }

    document.addBody("Ball", makeSphere(1.0));
    saveDocument(document, link);

    CHECK(std::filesystem::is_symlink(link));
    CHECK(loadDocument(real).document->bodies().size() == 2);
    CHECK(filesIn(directory.path()) == std::vector<std::string>{"real", "shortcut.lcad"});
    CHECK(filesIn(directory.file("real")) == std::vector<std::string>{"part.lcad"});
}

TEST_CASE("A save that cannot happen says why and leaves nothing behind", "[document][file]") {
    const TempDir directory;
    Document document;
    document.addBody("Box", makeBox(1.0, 2.0, 3.0));

    SECTION("into a folder that does not exist") {
        CHECK_THROWS_WITH(saveDocument(document, directory.file("missing") / "part.lcad"),
                          ContainsSubstring("does not exist"));
    }

    SECTION("onto a folder") {
        std::filesystem::create_directory(directory.file("folder.lcad"));
        CHECK_THROWS_WITH(saveDocument(document, directory.file("folder.lcad")),
                          ContainsSubstring("is a folder"));
        CHECK(filesIn(directory.path()) == std::vector<std::string>{"folder.lcad"});
    }

#if !defined(_WIN32)
    // A read-only folder on Windows still takes new files; its permissions are not Unix's.
    SECTION("into a folder that cannot be written to, keeping the existing file") {
        if (::geteuid() == 0) {
            // Not SKIP(): that marks the whole test case skipped, which older versions of
            // Catch2 report to CTest as a failure.
            WARN("not checked: root can write to a read-only folder; run as an ordinary user");
            return;
        }
        const std::filesystem::path path = directory.file("part.lcad");
        saveDocument(document, path);
        const std::string before = readFile(path);
        std::filesystem::permissions(directory.path(), std::filesystem::perms::owner_read |
                                                           std::filesystem::perms::owner_exec);

        document.addBody("Ball", makeSphere(1.0));
        CHECK_THROWS_AS(saveDocument(document, path), FileError);

        std::filesystem::permissions(directory.path(), std::filesystem::perms::owner_all);
        CHECK(readFile(path) == before);
        CHECK(filesIn(directory.path()) == std::vector<std::string>{"part.lcad"});
    }
#endif
}

TEST_CASE("Files that are not LCAD documents are refused with a reason", "[document][file]") {
    const TempDir directory;

    SECTION("no file at all") {
        CHECK_THAT(errorFrom(directory.file("nothing.lcad")),
                   ContainsSubstring("There is no file"));
    }
    SECTION("a folder") {
        CHECK_THAT(errorFrom(directory.path()), ContainsSubstring("is a folder"));
    }
    SECTION("an empty file") {
        writeFile(directory.file("empty.lcad"), "");
        CHECK_THAT(errorFrom(directory.file("empty.lcad")),
                   ContainsSubstring("not an LCAD document"));
    }
    SECTION("a text file") {
        writeFile(directory.file("notes.lcad"), "These are just some notes.\n");
        CHECK_THAT(errorFrom(directory.file("notes.lcad")),
                   ContainsSubstring("not an LCAD document"));
    }
    SECTION("some other zip file") {
        detail::writeArchive(directory.file("other.lcad"), {{"readme.txt", "hello"}});
        CHECK_THAT(errorFrom(directory.file("other.lcad")),
                   ContainsSubstring("not an LCAD document"));
    }
    SECTION("a zip whose manifest belongs to another format") {
        detail::writeArchive(directory.file("other.lcad"),
                             {{"manifest.json", R"({"format": "something-else"})"}});
        CHECK_THAT(errorFrom(directory.file("other.lcad")),
                   ContainsSubstring("not an LCAD document"));
    }
}

TEST_CASE("A document from a newer LCAD is refused, naming the version that can open it",
          "[document][file]") {
    const TempDir directory;
    saveDocument(*lcad::test::buildReferenceDocument(), directory.file("now.lcad"));
    writeVariant(directory.file("now.lcad"), directory.file("future.lcad"),
                 [](json& manifest, json&, detail::Entries&) {
                     manifest["formatVersion"] = currentFormatVersion + 1;
                     manifest["writtenBy"]["version"] = "9.8.7";
                 });

    const std::string error = errorFrom(directory.file("future.lcad"));

    CHECK_THAT(error, ContainsSubstring("LCAD 9.8.7"));
    CHECK_THAT(error,
               ContainsSubstring("format version " + std::to_string(currentFormatVersion + 1)));
    CHECK_THAT(error,
               ContainsSubstring("reads up to version " + std::to_string(currentFormatVersion)));
}

TEST_CASE("A document with damaged content is refused, never opened partially",
          "[document][file]") {
    using Change = std::function<void(json&, json&, detail::Entries&)>;
    struct Damage {
        const char* what;
        Change change;
        const char* expectedInMessage;
    };
    const Damage damage = GENERATE(
        Damage{"manifest is not JSON", [](json& m, json&, detail::Entries&) { m = "{ nope"; },
               "manifest.json is not valid JSON"},
        Damage{"format version is text",
               [](json& m, json&, detail::Entries&) { m["formatVersion"] = "one"; },
               "format version is not a whole number"},
        Damage{"format version is zero",
               [](json& m, json&, detail::Entries&) { m["formatVersion"] = 0; },
               "format version is 0"},
        Damage{"no units", [](json& m, json&, detail::Entries&) { m.erase("units"); },
               "has no \"units\""},
        Damage{"unknown length unit",
               [](json& m, json&, detail::Entries&) { m["units"]["length"] = "furlong"; },
               "length unit \"furlong\""},
        Damage{"no document.json", [](json&, json& d, detail::Entries&) { d = nullptr; },
               "has no document.json"},
        Damage{"document.json is not JSON", [](json&, json& d, detail::Entries&) { d = "[1, 2"; },
               "document.json is not valid JSON"},
        Damage{"no nextId", [](json&, json& d, detail::Entries&) { d.erase("nextId"); },
               "has no \"nextId\""},
        Damage{"bodies is not a list", [](json&, json& d, detail::Entries&) { d["bodies"] = 3; },
               "is not a list"},
        Damage{"a body is not an object",
               [](json&, json& d, detail::Entries&) { d["bodies"][1] = "Cylinder"; },
               "is not an object"},
        Damage{"a body has no name",
               [](json&, json& d, detail::Entries&) { d["bodies"][0].erase("name"); },
               "body 1 has no \"name\""},
        Damage{"a body's name is empty",
               [](json&, json& d, detail::Entries&) { d["bodies"][0]["name"] = ""; },
               "a name is needed"},
        Damage{"a body's id is text",
               [](json&, json& d, detail::Entries&) { d["bodies"][0]["id"] = "1"; },
               "is not a whole number"},
        Damage{"a body's id is negative",
               [](json&, json& d, detail::Entries&) { d["bodies"][0]["id"] = -1; },
               "is not a whole number"},
        Damage{"visibility is a number",
               [](json&, json& d, detail::Entries&) { d["bodies"][0]["visible"] = 1; },
               "is not true or false"},
        Damage{"two bodies share an id",
               [](json&, json& d, detail::Entries&) { d["bodies"][1]["id"] = 1; },
               "share the id 1"},
        Damage{"an id beyond nextId", [](json&, json& d, detail::Entries&) { d["nextId"] = 3; },
               "outside the ids"},
        Damage{"a body without geometry",
               [](json&, json& d, detail::Entries&) { d["bodies"][0].erase("geometry"); },
               "has no \"geometry\""},
        Damage{"geometry of an unknown kind",
               [](json&, json& d, detail::Entries&) {
                   d["bodies"][0]["geometry"]["role"] = "hologram";
               },
               "unknown kind \"hologram\""},
        Damage{"a geometry file is missing",
               [](json&, json&, detail::Entries& e) { e.erase("geometry/2.brep"); },
               "geometry of \"Cylinder\" (geometry/2.brep) is missing"},
        Damage{"a geometry file is not geometry",
               [](json&, json&, detail::Entries& e) { e["geometry/2.brep"] = "not a shape"; },
               "geometry of \"Cylinder\" cannot be read"},
        Damage{"a geometry file is cut short",
               [](json&, json&, detail::Entries& e) {
                   e["geometry/2.brep"].resize(e["geometry/2.brep"].size() / 2);
               },
               "geometry of \"Cylinder\" cannot be read"});
    CAPTURE(damage.what);
    const TempDir directory;
    saveDocument(*lcad::test::buildReferenceDocument(), directory.file("good.lcad"));
    writeVariant(directory.file("good.lcad"), directory.file("bad.lcad"), damage.change);

    const std::string error = errorFrom(directory.file("bad.lcad"));

    CHECK_THAT(error, ContainsSubstring(damage.expectedInMessage));
    CHECK_THAT(error, ContainsSubstring("bad.lcad"));
}

TEST_CASE("Entries and fields this version does not know are ignored", "[document][file]") {
    const TempDir directory;
    saveDocument(*lcad::test::buildReferenceDocument(), directory.file("good.lcad"));
    writeVariant(directory.file("good.lcad"), directory.file("extended.lcad"),
                 [](json& manifest, json& model, detail::Entries& entries) {
                     manifest["comment"] = "added by a later version";
                     model["bodies"][0]["color"] = "#ff8800";
                     entries["notes/readme.txt"] = "optional extra";
                 });

    const LoadedDocument loaded = loadDocument(directory.file("extended.lcad"));

    lcad::test::checkReferenceDocument(*loaded.document);
}

// Whatever happens to the bytes on disk, loading either fails with a FileError or gives the
// right geometry. The zip checksums are what make this hold.
TEST_CASE("A file with a flipped byte is refused or still read correctly", "[document][file]") {
    const TempDir directory;
    const std::filesystem::path good = directory.file("good.lcad");
    const std::filesystem::path bad = directory.file("bad.lcad");
    saveDocument(*lcad::test::buildReferenceDocument(), good);
    const std::string bytes = readFile(good);
    REQUIRE(bytes.size() > 1000);

    std::size_t refused = 0;
    std::size_t stillCorrect = 0;
    for (std::size_t position = 0; position < bytes.size(); position += 5) {
        std::string damaged = bytes;
        damaged[position] = static_cast<char>(damaged[position] ^ 0x5A);
        writeFile(bad, damaged);
        try {
            const LoadedDocument loaded = loadDocument(bad);
            INFO("byte " << position << " of " << bytes.size() << " flipped");
            lcad::test::checkReferenceDocument(*loaded.document);
            ++stillCorrect;
        } catch (const FileError&) {
            ++refused;
        }
    }
    // Nearly every byte matters; the few that do not are timestamps and the like.
    CHECK(refused > stillCorrect * 5);
}

TEST_CASE("A file cut short is refused", "[document][file]") {
    const TempDir directory;
    const std::filesystem::path good = directory.file("good.lcad");
    const std::filesystem::path bad = directory.file("bad.lcad");
    saveDocument(*lcad::test::buildReferenceDocument(), good);
    const std::string bytes = readFile(good);

    for (std::size_t keep = 0; keep < bytes.size(); keep += 97) {
        writeFile(bad, bytes.substr(0, keep));
        INFO("first " << keep << " of " << bytes.size() << " bytes kept");
        CHECK_THROWS_AS(loadDocument(bad), FileError);
    }
    writeFile(bad, bytes.substr(0, bytes.size() - 1));
    CHECK_THROWS_AS(loadDocument(bad), FileError);
}

TEST_CASE("A body whose geometry is not valid loads with a warning, not silently",
          "[document][file]") {
    const TempDir directory;
    const std::filesystem::path path = directory.file("open-box.lcad");
    DocumentState state;
    state.bodies.push_back(Body{ObjectId{1}, "Open Box", true, openBox()});
    state.bodies.push_back(Body{ObjectId{2}, "Good Box", true, makeBox(1.0, 1.0, 1.0)});
    state.nextId = 3;
    REQUIRE_FALSE(lcad::kernel::isValid(state.bodies[0].shape));
    saveDocument(Document(std::move(state)), path);
    std::vector<lcad::core::LogMessage> logged;
    const auto sink =
        lcad::core::addLogSink([&](const lcad::core::LogMessage& m) { logged.push_back(m); });

    const LoadedDocument loaded = loadDocument(path);
    lcad::core::removeLogSink(sink);

    REQUIRE(loaded.document->bodies().size() == 2);
    REQUIRE(loaded.warnings.size() == 1);
    CHECK_THAT(loaded.warnings[0], ContainsSubstring("\"Open Box\""));
    CHECK(lcad::kernel::countTopology(loaded.document->bodies()[0].shape).faces == 5);
    // The warning is in the log as well.
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].level == lcad::core::LogLevel::Warning);
    CHECK(logged[0].source == "document");
}

TEST_CASE("The preview image is stored and can be read on its own", "[document][file]") {
    const TempDir directory;
    const std::filesystem::path path = directory.file("with-preview.lcad");
    // Any bytes do; the document module does not interpret them.
    const std::string image = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(300, '\x7f');
    Document document;
    document.addBody("Box", makeBox(1.0, 2.0, 3.0));

    saveDocument(document, path, SaveOptions{image, {}, {}});

    CHECK(readThumbnail(path) == image);
    CHECK(loadDocument(path).document->bodies().size() == 1);

    SECTION("a document saved without one has none") {
        saveDocument(document, path);
        CHECK(readThumbnail(path).empty());
    }
    SECTION("asking a file that is not a document is an error") {
        CHECK_THROWS_AS(readThumbnail(directory.file("nothing.lcad")), FileError);
    }
}

TEST_CASE("The archive reader refuses more data than its limit", "[document][file]") {
    const TempDir directory;
    const std::filesystem::path path = directory.file("big.zip");
    detail::writeArchive(path,
                         {{"a.bin", std::string(600, 'a')}, {"b.bin", std::string(600, 'b')}});

    CHECK(detail::readArchive(path, 1200).size() == 2);
    CHECK_THROWS_WITH(detail::readArchive(path, 1199), ContainsSubstring("more data than"));
}

TEST_CASE("writeArchive does not overwrite, and cleans up after a failure", "[document][file]") {
    const TempDir directory;
    const std::filesystem::path path = directory.file("once.zip");
    detail::writeArchive(path, {{"a.txt", "first"}});

    CHECK_THROWS_AS(detail::writeArchive(path, {{"a.txt", "second"}}), FileError);
    CHECK(detail::readArchive(path).at("a.txt") == "first");
    CHECK_THROWS_AS(detail::writeArchive(directory.file("missing") / "x.zip", {{"a.txt", "x"}}),
                    FileError);
    CHECK(filesIn(directory.path()) == std::vector<std::string>{"once.zip"});
}

namespace {

// A plate pulled from a dimensioned rectangle with a round hole in it, and one corner rounded.
struct Modeled {
    std::unique_ptr<Document> document = std::make_unique<Document>();
    ObjectId sketch;
    ObjectId pull;
    ObjectId fillet;
    ObjectId body;
    lcad::sketch::ConstraintId width = 0;
};

Modeled buildModeledDocument() {
    using lcad::sketch::ConstraintKind;
    Modeled made;
    Document& document = *made.document;
    made.sketch = document.addSketch("Outline", lcad::kernel::planeXY());
    lcad::sketch::Sketch s;
    const auto r = lcad::sketch::addRectangle(s, {0.0, 0.0}, {60.0, 40.0});
    s.addConstraint(ConstraintKind::Coincident, s.find(r.bottom)->start, s.origin());
    made.width = s.addConstraint(ConstraintKind::Distance, s.find(r.bottom)->start,
                                 s.find(r.bottom)->end, 0, 60.0);
    s.addConstraint(ConstraintKind::Distance, s.find(r.right)->start, s.find(r.right)->end, 0,
                    40.0);
    s.addCircle({20.0, 20.0}, 6.0);
    s.solve();
    document.setSketchGeometry(made.sketch, s, "Draw");
    std::vector<lcad::sketch::EntityId> outline{r.bottom, r.right, r.top, r.left};
    std::sort(outline.begin(), outline.end());

    Feature pull;
    pull.kind = FeatureKind::Pull;
    pull.sketch = made.sketch;
    // The rectangle with the round hole in it, not the disc.
    pull.regions = {outline};
    pull.distance = 8.0;
    made.pull = document.addFeature(pull);
    made.body = document.findFeature(made.pull)->body;

    Feature fillet;
    fillet.kind = FeatureKind::Fillet;
    fillet.body = made.body;
    fillet.size = 5.0;
    for (const std::string& name : lcad::kernel::edgeNames(document.namedBody(made.body))) {
        if (name.find("/side/" + std::to_string(r.bottom) + "|") != std::string::npos &&
            name.find("/side/" + std::to_string(r.right) + "#") != std::string::npos) {
            fillet.edges.push_back(name);
        }
    }
    made.fillet = document.addFeature(fillet);
    return made;
}

const double pi = 3.14159265358979323846;
// The plate, less the hole, less what rounding one corner to 5 takes off.
const double modeledVolume = (2400.0 - pi * 36.0 - (1.0 - pi / 4.0) * 25.0) * 8.0;

double volumeOf(const Document& document, ObjectId body) {
    return lcad::kernel::volume(document.findBody(body)->shape);
}

} // namespace

TEST_CASE("A model reopens as its sketches and features, and is built again from them",
          "[document][file][feature]") {
    const TempDir directory;
    const std::filesystem::path path = directory.file("bracket.lcad");
    Modeled made = buildModeledDocument();
    REQUIRE(made.document->findFeature(made.fillet)->edges.size() == 1);
    REQUIRE(made.document->featureError(made.fillet).empty());
    REQUIRE_THAT(volumeOf(*made.document, made.body), WithinRel(modeledVolume, 1e-9));
    made.document->renameBody(made.body, "Plate");
    made.document->renameFeature(made.fillet, "Corner");

    saveDocument(*made.document, path);
    const LoadedDocument loaded = loadDocument(path);
    Document& document = *loaded.document;
    CHECK(loaded.warnings.empty());

    // The same things, in the same order, under the same ids and names.
    CHECK(document.timeline() == made.document->timeline());
    REQUIRE(document.features().size() == 2);
    CHECK(document.features() == made.document->features());
    CHECK(document.findFeature(made.fillet)->name == "Corner");
    CHECK(document.nextObjectId() == made.document->nextObjectId());
    // The body was built again, not read: same name, same measurements, same face names.
    const Body* body = document.findBody(made.body);
    REQUIRE(body != nullptr);
    CHECK(body->name == "Plate");
    CHECK(body->madeBy == made.pull);
    CHECK_THAT(volumeOf(document, made.body), WithinRel(modeledVolume, 1e-9));
    CHECK(body->faceNames == made.document->findBody(made.body)->faceNames);
    CHECK_FALSE(document.isModified());

    SECTION("it is still a model: change the sketch and it follows") {
        lcad::sketch::Sketch geometry = document.findSketch(made.sketch)->geometry;
        geometry.setConstraintValue(made.width, 80.0);
        REQUIRE(geometry.solve().status == lcad::sketch::SolveStatus::Solved);
        document.setSketchGeometry(made.sketch, geometry, "Change Dimension");
        CHECK(document.featureError(made.fillet).empty());
        CHECK_THAT(volumeOf(document, made.body),
                   WithinRel(modeledVolume + 20.0 * 40.0 * 8.0, 1e-9));
    }
    SECTION("in the file, features are readable and the body has no geometry of its own") {
        const auto entries = detail::readArchive(path);
        const json model = json::parse(entries.at("document.json"));
        REQUIRE(model["features"].size() == 2);
        CHECK(model["features"][0]["kind"] == "Pull");
        CHECK(model["features"][0]["operation"] == "NewBody");
        CHECK(model["features"][0]["distance"] == 8.0);
        CHECK(model["features"][0]["regions"].size() == 1);
        CHECK(model["features"][1]["kind"] == "Fillet");
        CHECK(model["features"][1]["edges"].size() == 1);
        CHECK(model["timeline"].size() == 3);
        CHECK_FALSE(model.contains("marker"));
        REQUIRE(model["bodies"].size() == 1);
        CHECK(model["bodies"][0]["madeBy"] == made.pull.value);
        CHECK_FALSE(model["bodies"][0].contains("geometry"));
        for (const auto& entry : entries) {
            CHECK(entry.first.find("geometry/") == std::string::npos);
        }
    }
}

TEST_CASE("The history marker, suppression, and failures are kept with the file",
          "[document][file][feature]") {
    const TempDir directory;
    const std::filesystem::path path = directory.file("state.lcad");
    Modeled made = buildModeledDocument();
    Document& document = *made.document;
    // A box that was put in as it is, with a feature on it.
    const ObjectId box = document.addBody("Box", makeBox(10.0, 10.0, 10.0));
    Feature bevel;
    bevel.kind = FeatureKind::Chamfer;
    bevel.body = box;
    bevel.size = 2.0;
    bevel.edges = {lcad::kernel::edgeNames(document.namedBody(box)).front()};
    const ObjectId chamfer = document.addFeature(bevel);
    REQUIRE(document.featureError(chamfer).empty());

    SECTION("a body that was put in keeps the shape it was given") {
        saveDocument(document, path);
        const LoadedDocument loaded = loadDocument(path);
        const Body* reopened = loaded.document->findBody(box);
        REQUIRE(reopened != nullptr);
        CHECK_THAT(lcad::kernel::volume(reopened->source), WithinRel(1000.0, 1e-9));
        // Beveled once, not twice: the feature was applied to what was stored.
        CHECK_THAT(lcad::kernel::volume(reopened->shape), WithinRel(1000.0 - 20.0, 1e-9));
        CHECK(loaded.warnings.empty());
    }
    SECTION("the marker and a suppressed feature") {
        document.setFeatureSuppressed(made.fillet, true);
        document.setHistoryMarker(4);
        saveDocument(document, path);
        const LoadedDocument loaded = loadDocument(path);
        CHECK(loaded.document->historyMarker() == 4);
        CHECK(loaded.document->findFeature(made.fillet)->suppressed);
        // The chamfer lies beyond the marker, so the box is as it was given.
        CHECK_THAT(volumeOf(*loaded.document, box), WithinRel(1000.0, 1e-9));
        CHECK(json::parse(detail::readArchive(path).at("document.json"))["marker"] == 4);
    }
    SECTION("a feature that cannot be applied is opened with a warning, not refused") {
        Feature fillet = *document.findFeature(made.fillet);
        fillet.size = 500.0;
        document.changeFeature(made.fillet, fillet);
        REQUIRE_FALSE(document.featureError(made.fillet).empty());
        saveDocument(document, path);
        const LoadedDocument loaded = loadDocument(path);
        REQUIRE(loaded.warnings.size() == 1);
        CHECK_THAT(loaded.warnings[0], ContainsSubstring("Fillet 1 could not be applied"));
        CHECK_THAT(loaded.warnings[0], ContainsSubstring("radius does not fit"));
        CHECK_FALSE(loaded.document->featureError(made.fillet).empty());
        // What can be built is there.
        CHECK_FALSE(loaded.document->findBody(made.body)->shape.isNull());
    }
}

TEST_CASE("A document with a damaged feature is refused", "[document][file][feature]") {
    using Change = std::function<void(json&)>;
    struct Damage {
        const char* what;
        Change change; // applied to the model
    };
    const Damage damage = GENERATE(
        Damage{"a kind this LCAD does not know",
               [](json& m) { m["features"][0]["kind"] = "Loft"; }},
        Damage{"an unknown operation", [](json& m) { m["features"][0]["operation"] = "Melt"; }},
        Damage{"a missing distance", [](json& m) { m["features"][0].erase("distance"); }},
        Damage{"a body that is not there", [](json& m) { m["features"][1]["body"] = 999; }},
        Damage{"edges that are not text", [](json& m) { m["features"][1]["edges"] = {1, 2}; }},
        Damage{"a timeline that leaves a feature out", [](json& m) { m["timeline"].erase(2); }},
        Damage{"a timeline with the pull before its sketch",
               [](json& m) { std::swap(m["timeline"][0], m["timeline"][1]); }},
        Damage{"a body made by a feature that makes none",
               [](json& m) { m["bodies"][0]["madeBy"] = m["features"][1]["id"]; }},
        Damage{"features that are not a list", [](json& m) { m["features"] = 3; }});
    CAPTURE(damage.what);
    const TempDir directory;
    const std::filesystem::path good = directory.file("good.lcad");
    const std::filesystem::path path = directory.file("damaged.lcad");
    const Modeled made = buildModeledDocument();
    saveDocument(*made.document, good);
    REQUIRE_NOTHROW(loadDocument(good));
    writeVariant(good, path,
                 [&damage](json&, json& model, detail::Entries&) { damage.change(model); });
    CHECK_THROWS_AS(loadDocument(path), FileError);
}
