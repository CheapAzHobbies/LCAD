// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/core/Path.h>
#include <lcad/document/Document.h>
#include <lcad/document/File.h>
#include <lcad/document/Links.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/Transform.h>

#include <TempDir.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>

using namespace lcad::document;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// A part file holding one box of the given height.
void writePart(const std::filesystem::path& path, double height) {
    Document part;
    const ObjectId box = part.addBody("Wheel", lcad::kernel::makeBox(10.0, 10.0, height));
    part.setBodyMaterial(box, "TPU");
    saveDocument(part, path);
}

// A document with the part at `file` linked into it.
ObjectId link(Document& document, const std::filesystem::path& file,
              const std::filesystem::path& documentPath) {
    return document.addLinkedComponent("Wheel", makeLink(file, documentPath.parent_path()),
                                       readLinkedContent(file));
}

const Body* bodyOf(const Document& document, ObjectId component) {
    for (const Body& body : document.bodies()) {
        if (body.component == component) {
            return &body;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("A stamp tells versions of a file apart", "[document][links]") {
    lcad::test::TempDir directory;
    const auto file = directory.file("part.lcad");
    writePart(file, 5.0);
    const std::string first = stampOf(file);
    CHECK(stampOf(file) == first);
    // The same bytes elsewhere have the same stamp.
    std::filesystem::copy_file(file, directory.file("copy.lcad"));
    CHECK(stampOf(directory.file("copy.lcad")) == first);
    writePart(file, 7.0);
    CHECK(stampOf(file) != first);
    CHECK_THROWS_AS(stampOf(directory.file("nothing.lcad")), FileError);
}

TEST_CASE("A linked document gives its shown bodies, placed as they are there",
          "[document][links]") {
    lcad::test::TempDir directory;
    const auto file = directory.file("assembly.lcad");
    {
        Document document;
        document.addBody("Base", lcad::kernel::makeBox(10.0, 10.0, 10.0));
        const ObjectId hidden = document.addBody("Hidden", lcad::kernel::makeBox(1.0, 1.0, 1.0));
        document.setBodyVisible(hidden, false);
        const ObjectId wheel =
            document.addComponent("Wheel", {}, lcad::kernel::translation({0.0, 0.0, 100.0}));
        document.addBody("Tire", lcad::kernel::makeBox(5.0, 5.0, 5.0), wheel);
        document.addOccurrence(wheel, {}, lcad::kernel::translation({50.0, 0.0, 0.0}));
        saveDocument(document, file);
    }

    const LinkedContent content = readLinkedContent(file);

    REQUIRE(content.bodies.size() == 3);
    CHECK(content.stamp == stampOf(file));
    CHECK(content.bodies[0].name == "Base");
    CHECK(content.bodies[1].name == "Wheel:1 Tire");
    CHECK(content.bodies[2].name == "Wheel:2 Tire");
    // One body of the component, appearing twice, under two keys.
    CHECK(content.bodies[1].key != content.bodies[2].key);
    CHECK_THAT(lcad::kernel::boundingBox(content.bodies[1].shape)->min.z, WithinAbs(100.0, 1e-9));
    CHECK_THAT(lcad::kernel::boundingBox(content.bodies[2].shape)->min.x, WithinAbs(50.0, 1e-9));
    CHECK_FALSE(content.bodies[0].faceNames.empty());
}

TEST_CASE("Links are kept relative to the document's folder", "[document][links]") {
    const std::filesystem::path folder = "/home/someone/projects/car";
    CHECK(makeLink("/home/someone/projects/car/parts/wheel.lcad", folder) == "parts/wheel.lcad");
    CHECK(makeLink("/home/someone/projects/common/bolt.lcad", folder) == "../common/bolt.lcad");
    // Whole, which on Windows takes a drive.
    const std::filesystem::path whole = std::filesystem::absolute("/home/someone/bolt.lcad");
    CHECK(makeLink(whole, {}) == lcad::core::toGenericUtf8(whole));
    CHECK(resolveLink("parts/wheel.lcad", folder) ==
          std::filesystem::path("/home/someone/projects/car/parts/wheel.lcad"));
    CHECK(resolveLink("../common/bolt.lcad", folder) ==
          std::filesystem::path("/home/someone/projects/common/bolt.lcad"));
    CHECK(resolveLink("/elsewhere/bolt.lcad", folder) ==
          std::filesystem::path("/elsewhere/bolt.lcad"));
}

TEST_CASE("Linked files are checked, and updated when they change", "[document][links]") {
    lcad::test::TempDir directory;
    std::filesystem::create_directories(directory.file("parts"));
    const auto part = directory.file("parts/wheel.lcad");
    const auto host = directory.file("car.lcad");
    writePart(part, 5.0);
    Document document;
    const ObjectId wheel = link(document, part, host);
    CHECK(document.findComponent(wheel)->link == "parts/wheel.lcad");
    CHECK(bodyOf(document, wheel)->material == "TPU");
    saveDocument(document, host);

    std::vector<LinkStatus> statuses = checkLinks(document, host);
    REQUIRE(statuses.size() == 1);
    CHECK(statuses.front().state == LinkStatus::State::UpToDate);
    CHECK(statuses.front().file == part);

    SECTION("a changed file is read again") {
        writePart(part, 9.0);
        statuses = checkLinks(document, host);
        CHECK(statuses.front().state == LinkStatus::State::Changed);
        const ObjectId body = bodyOf(document, wheel)->id;
        CHECK(updateLinks(document, host).empty());
        CHECK(bodyOf(document, wheel)->id == body);
        CHECK_THAT(lcad::kernel::volume(bodyOf(document, wheel)->shape), WithinRel(900.0, 1e-9));
        CHECK(checkLinks(document, host).front().state == LinkStatus::State::UpToDate);
        CHECK(document.undoLabel() == "Update Links");
    }
    SECTION("a missing file is reported, and the copy kept in the document is shown") {
        std::filesystem::remove(part);
        statuses = checkLinks(document, host);
        CHECK(statuses.front().state == LinkStatus::State::Missing);
        CHECK_THAT(statuses.front().problem, ContainsSubstring("wheel.lcad"));
        const std::vector<std::string> problems = updateLinks(document, host);
        REQUIRE(problems.size() == 1);
        CHECK_THAT(problems.front(), ContainsSubstring("Wheel could not be updated"));
        // Opened again, the document still shows the wheel.
        const LoadedDocument loaded = loadDocument(host);
        CHECK(lcad::kernel::volume(bodyOf(*loaded.document, wheel)->shape) > 0.0);
    }
    SECTION("a file that is not a document is reported") {
        std::ofstream(part) << "not a document";
        CHECK(checkLinks(document, host).front().state == LinkStatus::State::Unreadable);
    }
    SECTION("what a linked file links to is checked too") {
        const auto axle = directory.file("parts/axle.lcad");
        writePart(axle, 50.0);
        Document wheelWithAxle;
        wheelWithAxle.addBody("Wheel", lcad::kernel::makeBox(10.0, 10.0, 5.0));
        link(wheelWithAxle, axle, part);
        saveDocument(wheelWithAxle, part);
        statuses = checkLinks(document, host);
        REQUIRE(statuses.front().links.size() == 1);
        CHECK(statuses.front().links.front().file == axle);
        CHECK(statuses.front().links.front().state == LinkStatus::State::UpToDate);
    }
}

TEST_CASE("A project is packaged into one folder with its linked files", "[document][links]") {
    lcad::test::TempDir directory;
    std::filesystem::create_directories(directory.file("parts"));
    const auto axle = directory.file("parts/axle.lcad");
    const auto wheel = directory.file("parts/wheel.lcad");
    const auto host = directory.file("car.lcad");
    writePart(axle, 50.0);
    {
        Document wheelWithAxle;
        wheelWithAxle.addBody("Rim", lcad::kernel::makeBox(10.0, 10.0, 5.0));
        link(wheelWithAxle, axle, wheel);
        saveDocument(wheelWithAxle, wheel);
    }
    Document document;
    link(document, wheel, host);
    // The same wheel twice, and the axle on its own: each file is gathered once.
    link(document, wheel, host);
    link(document, axle, host);
    saveDocument(document, host);

    const auto folder = directory.file("package");
    const PackageResult result = packageProject(document, host, folder);

    CHECK(result.problems.empty());
    REQUIRE(result.files.size() == 3);
    CHECK(result.files.front() == folder / "car.lcad");
    CHECK(std::filesystem::exists(folder / "wheel.lcad"));
    CHECK(std::filesystem::exists(folder / "axle.lcad"));
    // Moved elsewhere whole, the package finds its own files, all up to date.
    const auto moved = directory.file("moved");
    std::filesystem::rename(folder, moved);
    const LoadedDocument loaded = loadDocument(moved / "car.lcad");
    const std::vector<LinkStatus> statuses = checkLinks(*loaded.document, moved / "car.lcad");
    REQUIRE(statuses.size() == 3);
    for (const LinkStatus& status : statuses) {
        CHECK(status.file.parent_path() == moved);
        CHECK(status.state == LinkStatus::State::UpToDate);
        for (const LinkStatus& inner : status.links) {
            CHECK(inner.file == moved / "axle.lcad");
            CHECK(inner.state == LinkStatus::State::UpToDate);
        }
    }
    // The document itself is not changed by packaging.
    CHECK(document.components().front().link == "parts/wheel.lcad");

    SECTION("a missing file is reported, and does not stop the rest") {
        std::filesystem::remove(axle);
        const PackageResult again = packageProject(document, host, directory.file("second"));
        CHECK_FALSE(again.problems.empty());
        CHECK(std::filesystem::exists(directory.file("second/car.lcad")));
    }
}
