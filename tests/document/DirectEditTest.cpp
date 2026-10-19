// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/document/Document.h>
#include <lcad/document/File.h>
#include <lcad/kernel/Check.h>
#include <lcad/kernel/Exchange.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/Plane.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/kernel/Transform.h>

#include <TempDir.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <functional>
#include <numbers>
#include <string>
#include <vector>

using namespace lcad::document;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double pi = std::numbers::pi;

// The name of the one face of a body that `pick` is true of.
std::string faceOf(const Document& document, ObjectId body,
                   const std::function<bool(const lcad::kernel::Shape&)>& pick) {
    const lcad::kernel::NamedShape named = document.namedBody(body);
    std::string found;
    for (std::size_t i = 0; i < named.faceNames.size(); ++i) {
        if (pick(lcad::kernel::subShape(named.shape, lcad::kernel::SubShapeKind::Face,
                                        static_cast<int>(i)))) {
            REQUIRE(found.empty());
            found = named.faceNames[i];
        }
    }
    REQUIRE_FALSE(found.empty());
    return found;
}

std::function<bool(const lcad::kernel::Shape&)> topAt(double z) {
    return [z](const lcad::kernel::Shape& face) {
        const auto plane = lcad::kernel::planeOfFace(face);
        return plane.has_value() && plane->normal.z > 0.999 && std::abs(plane->origin.z - z) < 1e-6;
    };
}

bool isRound(const lcad::kernel::Shape& face) {
    return lcad::kernel::radiusOf(face).has_value() && !lcad::kernel::planeOfFace(face).has_value();
}

// A STEP file with a plate 60 by 40 by 10 with a hole of radius 5 through it at (30, 20),
// written by another program as far as LCAD's document knows.
std::filesystem::path stepPlate(const lcad::test::TempDir& directory) {
    using namespace lcad::kernel;
    const NamedShape box = nameFaces(makeBox(60.0, 40.0, 10.0), "b");
    const NamedShape pin = nameFaces(translated(makeCylinder(5.0, 12.0), 30.0, 20.0, -1.0), "p");
    const auto path = directory.file("bracket.step");
    exportStep({{"Bracket", combine(box, pin, Combination::Cut).shape}}, path);
    return path;
}

ObjectId importInto(Document& document, const std::filesystem::path& path) {
    const lcad::kernel::ImportResult read = lcad::kernel::importStep(path);
    REQUIRE(read.bodies.size() == 1);
    Transaction transaction(document, "Import");
    const ObjectId body = document.addBody(read.bodies[0].name, read.bodies[0].shape);
    transaction.commit();
    return body;
}

Feature directEdit(FeatureKind kind, ObjectId body, std::vector<std::string> faces) {
    Feature feature;
    feature.kind = kind;
    feature.body = body;
    feature.faces = std::move(faces);
    return feature;
}

double volumeOf(const Document& document, ObjectId body) {
    return lcad::kernel::volume(document.findBody(body)->shape);
}

} // namespace

TEST_CASE("Acceptance: import a STEP solid, offset a face, save, export STEP, and check it",
          "[document][direct]") {
    const lcad::test::TempDir directory;
    Document document;
    const ObjectId body = importInto(document, stepPlate(directory));
    CHECK(document.findBody(body)->name == "Bracket");
    const double plate = 24000.0 - pi * 25.0 * 10.0;
    CHECK_THAT(volumeOf(document, body), WithinRel(plate, 1e-9));

    // The top face, 5 higher.
    Feature offset =
        directEdit(FeatureKind::OffsetFace, body, {faceOf(document, body, topAt(10.0))});
    offset.distance = 5.0;
    const ObjectId raised = document.addFeature(offset);
    REQUIRE(document.featureError(raised).empty());
    CHECK(document.findFeature(raised)->name == "Offset Face 1");
    // The imported body is what is changed: no new body.
    CHECK(document.bodies().size() == 1);
    CHECK(document.findFeature(raised)->body == body);
    const double thicker = 60.0 * 40.0 * 15.0 - pi * 25.0 * 15.0;
    CHECK_THAT(volumeOf(document, body), WithinRel(thicker, 1e-9));

    // Saved, and opened again.
    const auto saved = directory.file("bracket.lcad");
    saveDocument(document, saved);
    const LoadedDocument loaded = loadDocument(saved);
    CHECK(loaded.warnings.empty());
    CHECK(*loaded.document->findFeature(raised) == *document.findFeature(raised));
    CHECK_THAT(volumeOf(*loaded.document, body), WithinRel(thicker, 1e-9));

    // Exported to STEP from the opened document, read back, and checked.
    const auto exported = directory.file("bracket-thicker.step");
    lcad::kernel::exportStep({{"Bracket", loaded.document->findBody(body)->shape}}, exported);
    const lcad::kernel::ImportResult again = lcad::kernel::importStep(exported);
    REQUIRE(again.bodies.size() == 1);
    CHECK(again.notes.empty());
    CHECK(again.bodies[0].name == "Bracket");
    CHECK(lcad::kernel::checkGeometry(again.bodies[0].shape).empty());
    CHECK_THAT(lcad::kernel::volume(again.bodies[0].shape), WithinRel(thicker, 1e-9));
    const auto box = lcad::kernel::boundingBox(again.bodies[0].shape);
    REQUIRE(box.has_value());
    CHECK_THAT(box->max.z, WithinAbs(15.0, 1e-6));

    // Edited from the timeline: the new distance is used.
    offset = *document.findFeature(raised);
    offset.distance = 2.0;
    document.changeFeature(raised, offset);
    CHECK_THAT(volumeOf(document, body), WithinRel(60.0 * 40.0 * 12.0 - pi * 25.0 * 12.0, 1e-9));
    document.undo();
    CHECK_THAT(volumeOf(document, body), WithinRel(thicker, 1e-9));
    // Suppressed, the import is as it was read: the original is kept.
    document.setFeatureSuppressed(raised, true);
    CHECK_THAT(volumeOf(document, body), WithinRel(plate, 1e-9));
    document.undo();
}

TEST_CASE("Move Face, Delete Face, and Repair are timeline features too", "[document][direct]") {
    const lcad::test::TempDir directory;
    Document document;
    const ObjectId body = importInto(document, stepPlate(directory));
    const double plate = 24000.0 - pi * 25.0 * 10.0;
    const std::string wall = faceOf(document, body, isRound);

    // The hole, 15 along X.
    Feature move = directEdit(FeatureKind::MoveFace, body, {wall});
    move.reference = "X";
    move.distance = 15.0;
    const ObjectId moved = document.addFeature(move);
    REQUIRE(document.featureError(moved).empty());
    CHECK(document.findFeature(moved)->name == "Move Face 1");
    CHECK_THAT(volumeOf(document, body), WithinRel(plate, 1e-9));
    const std::string movedWall = faceOf(document, body, isRound);
    CHECK(movedWall == wall);
    const lcad::kernel::NamedShape now = document.namedBody(body);
    const auto axis = lcad::kernel::axisOf(lcad::kernel::subShape(
        now.shape, lcad::kernel::SubShapeKind::Face, lcad::kernel::findFace(now, wall)));
    REQUIRE(axis.has_value());
    CHECK_THAT(axis->origin.x, WithinAbs(45.0, 1e-9));
    CHECK_THAT(axis->origin.y, WithinAbs(20.0, 1e-9));

    // Then gone: the plate is whole.
    const ObjectId filled = document.addFeature(directEdit(FeatureKind::DeleteFace, body, {wall}));
    REQUIRE(document.featureError(filled).empty());
    CHECK_THAT(volumeOf(document, body), WithinRel(24000.0, 1e-9));
    // A face that cannot be taken out says so, and the rest stays.
    const ObjectId top = document.addFeature(
        directEdit(FeatureKind::DeleteFace, body, {faceOf(document, body, topAt(10.0))}));
    CHECK_THAT(document.featureError(top),
               ContainsSubstring("The faces around them cannot be extended to close the gap."));
    CHECK_THAT(volumeOf(document, body), WithinRel(24000.0, 1e-9));
    document.undo();

    // The hole moved off the plate is still a move: it leaves a notch.
    move = *document.findFeature(moved);
    move.distance = 30.0;
    document.changeFeature(moved, move);
    CHECK(document.featureError(moved).empty());
    // A face that a feature works on and is gone: deleting the wall first.
    document.undo();
    Feature first = directEdit(FeatureKind::DeleteFace, body, {wall});
    const ObjectId early = document.addFeature(first);
    document.moveInTimeline(early, 1);
    CHECK_THAT(document.featureError(moved), ContainsSubstring("no longer exists"));
    document.undo();
    document.undo();

    // Repair leaves a sound body as it is.
    Feature repair;
    repair.kind = FeatureKind::Repair;
    repair.body = body;
    const ObjectId fixed = document.addFeature(repair);
    CHECK(document.featureError(fixed).empty());
    CHECK(document.findFeature(fixed)->name == "Repair 1");
    CHECK_THAT(volumeOf(document, body), WithinRel(24000.0, 1e-9));

    // And makes a solid of the faces of a box that were put in loose, keeping their names.
    const lcad::kernel::Shape box = lcad::kernel::makeBox(10.0, 20.0, 30.0);
    std::vector<lcad::kernel::Shape> pieces;
    for (int i = 0; i < lcad::kernel::subShapeCount(box, lcad::kernel::SubShapeKind::Face); ++i) {
        pieces.push_back(lcad::kernel::subShape(box, lcad::kernel::SubShapeKind::Face, i));
    }
    const ObjectId loose = document.addBody("Loose", lcad::kernel::makeCompound(pieces));
    CHECK_FALSE(lcad::kernel::checkGeometry(document.findBody(loose)->shape).empty());
    const std::vector<std::string> names = document.namedBody(loose).faceNames;
    repair.body = loose;
    const ObjectId sewn = document.addFeature(repair);
    REQUIRE(document.featureError(sewn).empty());
    CHECK(lcad::kernel::countTopology(document.findBody(loose)->shape).solids == 1);
    CHECK(lcad::kernel::checkGeometry(document.findBody(loose)->shape).empty());
    CHECK_THAT(volumeOf(document, loose), WithinRel(6000.0, 1e-9));
    for (const std::string& name : names) {
        CHECK(lcad::kernel::findFace(document.namedBody(loose), name) >= 0);
    }
    // An offset of one of its faces, after the repair, works on the solid.
    Feature longer =
        directEdit(FeatureKind::OffsetFace, loose, {faceOf(document, loose, topAt(30.0))});
    longer.distance = 10.0;
    const ObjectId stretched = document.addFeature(longer);
    REQUIRE(document.featureError(stretched).empty());
    CHECK_THAT(volumeOf(document, loose), WithinRel(8000.0, 1e-9));
    // Without the repair it does not.
    document.setFeatureSuppressed(sewn, true);
    CHECK_THAT(document.featureError(stretched), ContainsSubstring("not a solid"));
    document.undo();

    // All of it saved and opened again.
    const auto saved = directory.file("edits.lcad");
    saveDocument(document, saved);
    const LoadedDocument loaded = loadDocument(saved);
    CHECK(loaded.warnings.empty());
    for (const ObjectId id : {moved, filled, fixed, sewn, stretched}) {
        CHECK(*loaded.document->findFeature(id) == *document.findFeature(id));
    }
    CHECK_THAT(volumeOf(*loaded.document, body), WithinRel(24000.0, 1e-9));
    CHECK_THAT(volumeOf(*loaded.document, loose), WithinRel(8000.0, 1e-9));
}
