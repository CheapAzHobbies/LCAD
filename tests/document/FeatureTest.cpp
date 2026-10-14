// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/document/Document.h>
#include <lcad/document/File.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/Plane.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/sketch/Edit.h>

#include <TempDir.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

using namespace lcad::document;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using lcad::sketch::ConstraintKind;

namespace {

const double pi = 3.14159265358979323846;

// A document with the makings of a bracket: a fully dimensioned plate outline on the XY plane,
// and, for a hole, a circle in a second sketch on the same plane.
struct Bracket {
    Document document;
    ObjectId outline;
    ObjectId holes;
    lcad::sketch::ConstraintId width = 0;
    lcad::sketch::ConstraintId height = 0;
    lcad::sketch::EntityId circle = 0;
    lcad::sketch::EntityId bottom = 0;
    ObjectId plate;  // the Pull that makes the plate
    ObjectId hole;   // the Pull that cuts the hole
    ObjectId rounds; // the Fillet on the plate's four upright edges

    [[nodiscard]] const Body& body() const {
        return *document.findBody(document.findFeature(plate)->body);
    }
    [[nodiscard]] double volume() const { return lcad::kernel::volume(body().shape); }
    void setWidth(double value) {
        lcad::sketch::Sketch geometry = document.findSketch(outline)->geometry;
        REQUIRE(lcad::sketch::tryDimensionValue(geometry, width, value).ok());
        document.setSketchGeometry(outline, geometry, "Change Dimension");
    }
};

// The plate outline: `w` by `h` from the origin, held there, with both sizes dimensioned.
void drawOutline(Bracket& made, double w, double h) {
    made.outline = made.document.addSketch("Outline", lcad::kernel::planeXY());
    lcad::sketch::Sketch s;
    const lcad::sketch::RectangleParts r = lcad::sketch::addRectangle(s, {0.0, 0.0}, {w, h});
    made.bottom = r.bottom;
    s.addConstraint(ConstraintKind::Coincident, s.find(r.bottom)->start, s.origin());
    made.width = s.addConstraint(ConstraintKind::Distance, s.find(r.bottom)->start,
                                 s.find(r.bottom)->end, 0, w);
    made.height = s.addConstraint(ConstraintKind::Distance, s.find(r.right)->start,
                                  s.find(r.right)->end, 0, h);
    REQUIRE(s.solve().isFullyConstrained());
    made.document.setSketchGeometry(made.outline, s, "Rectangle");
}

Feature pullOf(ObjectId sketch, double distance, Operation operation = Operation::NewBody,
               ObjectId body = {}) {
    Feature feature;
    feature.kind = FeatureKind::Pull;
    feature.sketch = sketch;
    feature.distance = distance;
    feature.operation = operation;
    feature.body = body;
    return feature;
}

// The names of the plate's four upright edges: those between two of the pull's side faces.
std::vector<std::string> uprightEdges(const Document& document, ObjectId body) {
    std::vector<std::string> found;
    for (const std::string& name : lcad::kernel::edgeNames(document.namedBody(body))) {
        const std::size_t bar = name.find('|');
        if (bar != std::string::npos && name.find("/side/") < bar &&
            name.find("/side/", bar) != std::string::npos) {
            found.push_back(name);
        }
    }
    return found;
}

// The whole bracket: a 60 by 40 plate 8 thick, a hole of radius 6 through it at (20, 20), and
// its four corners rounded to 5.
void makeBracket(Bracket& made) {
    drawOutline(made, 60.0, 40.0);
    made.plate = made.document.addFeature(pullOf(made.outline, 8.0));
    const ObjectId body = made.document.findFeature(made.plate)->body;

    made.holes = made.document.addSketch("Holes", lcad::kernel::planeXY());
    lcad::sketch::Sketch s;
    made.circle = s.addCircle({20.0, 20.0}, 6.0);
    made.document.setSketchGeometry(made.holes, s, "Circle");
    made.hole = made.document.addFeature(pullOf(made.holes, 8.0, Operation::Cut, body));

    Feature rounds;
    rounds.kind = FeatureKind::Fillet;
    rounds.body = body;
    rounds.edges = uprightEdges(made.document, body);
    REQUIRE(rounds.edges.size() == 4);
    rounds.size = 5.0;
    made.rounds = made.document.addFeature(rounds);
}

// What the bracket's volume should be for a plate of `w` by `h`.
double bracketVolume(double w, double h) {
    return (w * h - (4.0 - pi) * 25.0 - pi * 36.0) * 8.0;
}

} // namespace

TEST_CASE("A pull makes a body from a sketch, as one undoable step", "[document][feature]") {
    Bracket made;
    drawOutline(made, 60.0, 40.0);
    Document& document = made.document;
    std::vector<Change::Kind> heard;
    document.addListener([&heard](const Change& change) { heard.push_back(change.kind); });

    const ObjectId id = document.addFeature(pullOf(made.outline, 8.0));
    made.plate = id;
    REQUIRE(document.features().size() == 1);
    const Feature& feature = *document.findFeature(id);
    CHECK(feature.name == "Pull 1");
    CHECK(document.featureError(id).empty());
    CHECK(document.undoLabel() == "Pull");
    // It made a body, which is listed like any other and knows where it comes from.
    REQUIRE(document.bodies().size() == 1);
    const Body& body = document.bodies()[0];
    CHECK(body.id == feature.body);
    CHECK(body.name == "Body");
    CHECK(body.madeBy == id);
    CHECK(body.visible);
    CHECK_THAT(lcad::kernel::volume(body.shape), WithinRel(60.0 * 40.0 * 8.0, 1e-9));
    CHECK(body.faceNames.size() == 6);
    // The timeline has the sketch, then the pull. The body is what they give, not an entry.
    CHECK(document.timeline() == std::vector<ObjectId>{made.outline, id});
    CHECK(document.kindOf(made.outline) == ItemKind::Sketch);
    CHECK(document.kindOf(id) == ItemKind::Feature);
    CHECK(document.historyMarker() == 2);
    // Listeners heard of the body, of its shape, and of the timeline.
    CHECK(std::count(heard.begin(), heard.end(), Change::Kind::BodyAdded) == 1);
    CHECK(std::count(heard.begin(), heard.end(), Change::Kind::BodyGeometryChanged) == 1);
    CHECK(std::count(heard.begin(), heard.end(), Change::Kind::TimelineChanged) >= 1);
    CHECK(heard.back() == Change::Kind::HistoryChanged);

    // (References into the document end with the next edit; keep the id.)
    const ObjectId bodyId = feature.body;
    document.undo();
    CHECK(document.features().empty());
    CHECK(document.bodies().empty());
    CHECK(document.timeline() == std::vector<ObjectId>{made.outline});
    document.redo();
    REQUIRE(document.findFeature(id) != nullptr);
    REQUIRE(document.findBody(bodyId) != nullptr);
    CHECK(document.findFeature(id)->body == bodyId);
    CHECK_THAT(made.volume(), WithinRel(19200.0, 1e-9));

    // A second pull is "Pull 2" and makes "Body 2".
    const ObjectId second = document.addFeature(pullOf(made.outline, -3.0));
    CHECK(document.findFeature(second)->name == "Pull 2");
    CHECK(document.findBody(document.findFeature(second)->body)->name == "Body 2");
    CHECK_THAT(
        lcad::kernel::boundingBox(document.findBody(document.findFeature(second)->body)->shape)
            ->min.z,
        WithinAbs(-3.0, 1e-9));

    // What it names has to be there.
    CHECK_THROWS_AS(document.addFeature(pullOf(ObjectId{999}, 5.0)), std::invalid_argument);
    CHECK_THROWS_AS(document.addFeature(pullOf(made.outline, 5.0, Operation::Join, ObjectId{999})),
                    std::invalid_argument);
    CHECK(document.features().size() == 2);
}

// The acceptance test of Phase 3, at the level of the document: build a dimensioned bracket,
// edit the first sketch, and verify that the features after it update.
TEST_CASE("A bracket follows the sketch it was built on", "[document][feature]") {
    Bracket made;
    makeBracket(made);
    Document& document = made.document;
    CHECK(document.timeline() ==
          std::vector<ObjectId>{made.outline, made.plate, made.holes, made.hole, made.rounds});
    for (const ObjectId feature : {made.plate, made.hole, made.rounds}) {
        CHECK(document.featureError(feature).empty());
    }
    REQUIRE(document.bodies().size() == 1);
    CHECK_THAT(made.volume(), WithinRel(bracketVolume(60.0, 40.0), 1e-9));
    CHECK(lcad::kernel::isValid(made.body().shape));

    // Change the width of the plate in the first sketch. The hole stays where it is, the
    // rounded corners move out with the corners, and nothing fails.
    made.setWidth(85.0);
    for (const ObjectId feature : {made.plate, made.hole, made.rounds}) {
        CHECK(document.featureError(feature).empty());
    }
    CHECK_THAT(made.volume(), WithinRel(bracketVolume(85.0, 40.0), 1e-9));
    const auto box = lcad::kernel::boundingBox(made.body().shape);
    REQUIRE(box.has_value());
    CHECK_THAT(box->max.x, WithinAbs(85.0, 1e-6));
    CHECK_THAT(box->max.z, WithinAbs(8.0, 1e-6));
    // The fillet still holds on to the edges it rounded: the names it keeps are the same.
    CHECK(document.findFeature(made.rounds)->edges.size() == 4);

    // One undo step puts the sketch and everything built on it back.
    document.undo();
    CHECK_THAT(made.volume(), WithinRel(bracketVolume(60.0, 40.0), 1e-9));
    document.redo();
    CHECK_THAT(made.volume(), WithinRel(bracketVolume(85.0, 40.0), 1e-9));
}

TEST_CASE("A feature that cannot be applied says why, and the model carries on without it",
          "[document][feature]") {
    Bracket made;
    makeBracket(made);
    Document& document = made.document;

    SECTION("a fillet too large for a plate that was made narrower") {
        // 9 wide leaves no room for two corners of radius 5.
        made.setWidth(9.0);
        CHECK(document.featureError(made.plate).empty());
        CHECK_THAT(document.featureError(made.rounds),
                   ContainsSubstring("radius does not fit these edges"));
        // The hole no longer meets the plate either, and says so.
        CHECK_THAT(document.featureError(made.hole),
                   ContainsSubstring("does not reach Body, so there is nothing to cut away"));
        // The body is what the features that still work make of it: the bare plate.
        CHECK_THAT(made.volume(), WithinRel(9.0 * 40.0 * 8.0, 1e-9));
        // Put right, everything is applied again without anything having been lost.
        made.setWidth(60.0);
        CHECK(document.featureError(made.rounds).empty());
        CHECK(document.featureError(made.hole).empty());
        CHECK_THAT(made.volume(), WithinRel(bracketVolume(60.0, 40.0), 1e-9));
    }
    SECTION("an outline that is no longer closed") {
        lcad::sketch::Sketch geometry = document.findSketch(made.outline)->geometry;
        geometry.remove(made.bottom);
        document.setSketchGeometry(made.outline, geometry, "Delete");
        CHECK_THAT(document.featureError(made.plate), ContainsSubstring("no closed outline"));
        // Without the plate there is no body for the others to work on, and each says which.
        CHECK(made.body().shape.isNull());
        CHECK_THAT(document.featureError(made.hole),
                   ContainsSubstring("Body, does not exist at this point"));
        CHECK_THAT(document.featureError(made.rounds), ContainsSubstring("does not exist"));
        // The features are all still there to be put right, and undo does that.
        CHECK(document.features().size() == 3);
        document.undo();
        CHECK(document.featureError(made.plate).empty());
        CHECK_THAT(made.volume(), WithinRel(bracketVolume(60.0, 40.0), 1e-9));
    }
    SECTION("an edge that is gone is named") {
        // Pull only the plate again, this time from a sketch with other curves: the edges the
        // fillet knows are not there any more.
        Feature changed = *document.findFeature(made.plate);
        changed.sketch = made.holes;
        document.changeFeature(made.plate, changed);
        CHECK(document.featureError(made.plate).empty());
        const std::string why = document.featureError(made.rounds);
        CHECK_THAT(why, ContainsSubstring("An edge it works on no longer exists"));
        CHECK_THAT(why,
                   ContainsSubstring("the edge between a side of Pull 1 and a side of Pull 1"));
    }
    SECTION("a sketch that is deleted") {
        document.removeSketch(made.holes);
        CHECK_THAT(document.featureError(made.hole), ContainsSubstring("sketch it was made from"));
        // The plate and its rounded corners do not depend on it.
        CHECK(document.featureError(made.rounds).empty());
        CHECK_THAT(made.volume(), WithinRel((2400.0 - (4.0 - pi) * 25.0) * 8.0, 1e-9));
    }
}

TEST_CASE("Features are suppressed, restored, changed, and removed", "[document][feature]") {
    Bracket made;
    makeBracket(made);
    Document& document = made.document;

    document.setFeatureSuppressed(made.rounds, true);
    CHECK(document.findFeature(made.rounds)->suppressed);
    CHECK(document.featureError(made.rounds).empty());
    CHECK_THAT(made.volume(), WithinRel((2400.0 - pi * 36.0) * 8.0, 1e-9));
    CHECK(document.undoLabel() == "Suppress Fillet 1");
    document.setFeatureSuppressed(made.rounds, false);
    CHECK_THAT(made.volume(), WithinRel(bracketVolume(60.0, 40.0), 1e-9));

    // A larger radius, and a thicker plate.
    Feature rounds = *document.findFeature(made.rounds);
    rounds.size = 8.0;
    document.changeFeature(made.rounds, rounds);
    CHECK_THAT(made.volume(), WithinRel((2400.0 - (4.0 - pi) * 64.0 - pi * 36.0) * 8.0, 1e-9));
    CHECK(document.undoLabel() == "Edit Fillet 1");
    document.undo();
    CHECK_THAT(made.volume(), WithinRel(bracketVolume(60.0, 40.0), 1e-9));

    document.renameFeature(made.hole, "Mounting Hole");
    CHECK(document.findFeature(made.hole)->name == "Mounting Hole");
    CHECK_THROWS_AS(document.renameFeature(made.hole, "  "), std::invalid_argument);

    // Without the hole.
    document.removeFeature(made.hole);
    CHECK(document.findFeature(made.hole) == nullptr);
    CHECK(document.timeline() ==
          std::vector<ObjectId>{made.outline, made.plate, made.holes, made.rounds});
    CHECK_THAT(made.volume(), WithinRel((2400.0 - (4.0 - pi) * 25.0) * 8.0, 1e-9));
    document.undo();
    CHECK(document.timeline() ==
          std::vector<ObjectId>{made.outline, made.plate, made.holes, made.hole, made.rounds});
    CHECK_THAT(made.volume(), WithinRel(bracketVolume(60.0, 40.0), 1e-9));

    // Removing the body removes what makes it and what works on it, as one step.
    const ObjectId body = document.findFeature(made.plate)->body;
    document.removeBody(body);
    CHECK(document.bodies().empty());
    CHECK(document.features().empty());
    CHECK(document.sketches().size() == 2);
    CHECK(document.undoLabel() == "Delete Body");
    document.undo();
    CHECK(document.features().size() == 3);
    CHECK_THAT(made.volume(), WithinRel(bracketVolume(60.0, 40.0), 1e-9));

    CHECK_THROWS_AS(document.removeFeature(ObjectId{999}), std::invalid_argument);
    CHECK_THROWS_AS(document.changeFeature(ObjectId{999}, rounds), std::invalid_argument);
    CHECK_THROWS_AS(document.setFeatureSuppressed(ObjectId{999}, true), std::invalid_argument);
}

TEST_CASE("The history marker, and editing a feature, show the model as it was",
          "[document][feature]") {
    Bracket made;
    makeBracket(made);
    Document& document = made.document;
    CHECK(document.historyMarker() == 5);

    // Back to after the plate was pulled: no hole, no rounded corners.
    document.setHistoryMarker(2);
    CHECK(document.historyMarker() == 2);
    CHECK_THAT(made.volume(), WithinRel(19200.0, 1e-9));
    CHECK(document.featureError(made.rounds).empty());
    // Back to the very start: the body does not exist yet.
    document.setHistoryMarker(0);
    CHECK(made.body().shape.isNull());
    document.undo();
    CHECK_THAT(made.volume(), WithinRel(19200.0, 1e-9));

    // What is added now goes in where the marker is, and is applied.
    Feature bevel;
    bevel.kind = FeatureKind::Chamfer;
    bevel.body = made.body().id;
    bevel.edges = {uprightEdges(document, made.body().id).front()};
    bevel.size = 2.0;
    const ObjectId added = document.addFeature(bevel);
    CHECK(document.timeline() == std::vector<ObjectId>{made.outline, made.plate, added, made.holes,
                                                       made.hole, made.rounds});
    CHECK(document.historyMarker() == 3);
    CHECK_THAT(made.volume(), WithinRel(19200.0 - 2.0 * 8.0, 1e-9));
    // Forward to the end: the corner that was beveled can no longer be rounded, which the
    // fillet reports while the rest is applied.
    document.setHistoryMarker(99);
    CHECK(document.historyMarker() == 6);
    CHECK_THAT(document.featureError(made.rounds), ContainsSubstring("no longer exists"));
    document.undo();
    document.undo();
    document.undo();
    CHECK(document.historyMarker() == 5);
    CHECK_THAT(made.volume(), WithinRel(bracketVolume(60.0, 40.0), 1e-9));

    // Editing a feature shows the model as it is just before it, so that what it refers to
    // can be seen. That is not an edit: it leaves nothing to undo.
    const std::string lastStep = document.undoLabel();
    const bool modified = document.isModified();
    document.setEditedFeature(made.rounds);
    CHECK(document.editedFeature() == made.rounds);
    CHECK_THAT(made.volume(), WithinRel((2400.0 - pi * 36.0) * 8.0, 1e-9));
    // The edges it rounds are there to be found by name.
    const lcad::kernel::NamedShape before = document.namedBody(made.body().id);
    for (const std::string& edge : document.findFeature(made.rounds)->edges) {
        CHECK(lcad::kernel::findEdge(before, edge) >= 0);
    }
    document.setEditedFeature({});
    CHECK_THAT(made.volume(), WithinRel(bracketVolume(60.0, 40.0), 1e-9));
    CHECK(document.undoLabel() == lastStep);
    CHECK(document.isModified() == modified);
    CHECK_THROWS_AS(document.setEditedFeature(ObjectId{999}), std::invalid_argument);
}

TEST_CASE("The timeline can be reordered only where nothing depends on the order",
          "[document][feature]") {
    Bracket made;
    makeBracket(made);
    Document& document = made.document;
    const std::vector<ObjectId> before = document.timeline();

    // The fillet works on the body the pull makes.
    try {
        document.moveInTimeline(made.rounds, 0);
        FAIL("the fillet cannot come before the pull");
    } catch (const std::invalid_argument& refusal) {
        CHECK_THAT(refusal.what(), ContainsSubstring("Fillet 1 works on Body, which Pull 1 makes"));
    }
    // A pull cannot come before its sketch.
    try {
        document.moveInTimeline(made.plate, 0);
        FAIL("the pull cannot come before its sketch");
    } catch (const std::invalid_argument& refusal) {
        CHECK_THAT(refusal.what(), ContainsSubstring("Pull 1 is made from Outline"));
    }
    CHECK(document.timeline() == before);

    // Rounding the corners before cutting the hole is allowed, and gives the same bracket.
    document.moveInTimeline(made.rounds, 2);
    CHECK(document.timeline() ==
          std::vector<ObjectId>{made.outline, made.plate, made.rounds, made.holes, made.hole});
    CHECK(document.featureError(made.rounds).empty());
    CHECK(document.featureError(made.hole).empty());
    CHECK_THAT(made.volume(), WithinRel(bracketVolume(60.0, 40.0), 1e-9));
    document.undo();
    CHECK(document.timeline() == before);
    CHECK_THROWS_AS(document.moveInTimeline(ObjectId{999}, 0), std::invalid_argument);
}

TEST_CASE("Only what depends on a change is built again", "[document][feature]") {
    Bracket made;
    drawOutline(made, 60.0, 40.0);
    Document& document = made.document;
    const ObjectId plate = document.addFeature(pullOf(made.outline, 8.0));
    // A second body from a second sketch, with a feature of its own.
    const ObjectId other = document.addSketch("Disc", lcad::kernel::planeXY());
    lcad::sketch::Sketch s;
    s.addCircle({200.0, 0.0}, 10.0);
    document.setSketchGeometry(other, s, "Circle");
    const ObjectId disc = document.addFeature(pullOf(other, 5.0));
    const ObjectId plateBody = document.findFeature(plate)->body;
    const ObjectId discBody = document.findFeature(disc)->body;

    const auto* plateBefore = document.findBody(plateBody)->shape.impl();
    const auto* discBefore = document.findBody(discBody)->shape.impl();
    std::vector<ObjectId> reshaped;
    document.addListener([&reshaped](const Change& change) {
        if (change.kind == Change::Kind::BodyGeometryChanged) {
            reshaped.push_back(change.body);
        }
    });
    made.setWidth(70.0);
    // The plate is a new shape. The disc is the very shape it was, and nobody was told
    // otherwise.
    CHECK(document.findBody(plateBody)->shape.impl() != plateBefore);
    CHECK(document.findBody(discBody)->shape.impl() == discBefore);
    CHECK(reshaped == std::vector<ObjectId>{plateBody});

    // While rebuilding is suspended, edits are taken and the bodies wait.
    document.suspendRebuild();
    made.setWidth(90.0);
    made.setWidth(100.0);
    CHECK_THAT(lcad::kernel::volume(document.findBody(plateBody)->shape),
               WithinRel(70.0 * 40.0 * 8.0, 1e-9));
    document.resumeRebuild();
    CHECK_THAT(lcad::kernel::volume(document.findBody(plateBody)->shape),
               WithinRel(100.0 * 40.0 * 8.0, 1e-9));
    CHECK(reshaped == std::vector<ObjectId>{plateBody, plateBody});
    CHECK_THROWS_AS(document.resumeRebuild(), std::logic_error);
}

TEST_CASE("Features work on bodies that were put in as they are", "[document][feature]") {
    Document document;
    const ObjectId box = document.addBody("Box", lcad::kernel::makeBox(50.0, 30.0, 20.0));
    // A boss on top of the box, joined to it.
    lcad::kernel::Plane top = lcad::kernel::planeXY();
    top.origin = {0.0, 0.0, 20.0};
    const ObjectId sketch = document.addSketch("Sketch 1", top);
    lcad::sketch::Sketch s;
    const lcad::sketch::EntityId circle = s.addCircle({25.0, 15.0}, 5.0);
    document.setSketchGeometry(sketch, s, "Circle");
    const ObjectId boss = document.addFeature(pullOf(sketch, 10.0, Operation::Join, box));

    CHECK(document.featureError(boss).empty());
    CHECK(document.bodies().size() == 1);
    CHECK(document.timeline() == std::vector<ObjectId>{box, sketch, boss});
    CHECK(document.kindOf(box) == ItemKind::Body);
    const Body& body = *document.findBody(box);
    CHECK_THAT(lcad::kernel::volume(body.shape), WithinRel(30000.0 + pi * 25.0 * 10.0, 1e-9));
    // The body keeps the shape it was given, which is what a feature before the boss sees.
    CHECK_THAT(lcad::kernel::volume(body.source), WithinRel(30000.0, 1e-9));
    document.setHistoryMarker(1);
    CHECK_THAT(lcad::kernel::volume(document.findBody(box)->shape), WithinRel(30000.0, 1e-9));
    document.undo();

    // Turned into a cut, a blind hole. Turned into a body of its own, a second body.
    Feature changed = *document.findFeature(boss);
    changed.operation = Operation::Cut;
    changed.distance = -8.0;
    document.changeFeature(boss, changed);
    CHECK_THAT(lcad::kernel::volume(document.findBody(box)->shape),
               WithinRel(30000.0 - pi * 25.0 * 8.0, 1e-9));
    changed.operation = Operation::NewBody;
    changed.distance = 10.0;
    document.changeFeature(boss, changed);
    REQUIRE(document.bodies().size() == 2);
    const ObjectId made = document.findFeature(boss)->body;
    CHECK(made != box);
    CHECK(document.findBody(made)->madeBy == boss);
    CHECK_THAT(lcad::kernel::volume(document.findBody(made)->shape),
               WithinRel(pi * 25.0 * 10.0, 1e-9));
    CHECK_THAT(lcad::kernel::volume(document.findBody(box)->shape), WithinRel(30000.0, 1e-9));
    // And back: the body it made goes, as part of the same step.
    document.undo();
    CHECK(document.bodies().size() == 1);
    CHECK(document.findFeature(boss)->body == box);
    document.redo();
    changed.operation = Operation::Join;
    changed.body = box;
    document.changeFeature(boss, changed);
    CHECK(document.bodies().size() == 1);
    CHECK_THAT(lcad::kernel::volume(document.findBody(box)->shape),
               WithinRel(30000.0 + pi * 25.0 * 10.0, 1e-9));

    // The edge where the boss meets the box can be rounded, by its name.
    Feature rounds;
    rounds.kind = FeatureKind::Fillet;
    rounds.body = box;
    rounds.size = 2.0;
    for (const std::string& name : lcad::kernel::edgeNames(document.namedBody(box))) {
        if (name.find("/side/" + std::to_string(circle)) != std::string::npos &&
            name.find("/f") != std::string::npos) {
            rounds.edges.push_back(name);
        }
    }
    REQUIRE(rounds.edges.size() == 1);
    const ObjectId fillet = document.addFeature(rounds);
    CHECK(document.featureError(fillet).empty());
    CHECK(lcad::kernel::volume(document.findBody(box)->shape) > 30000.0 + pi * 25.0 * 10.0);

    // Removing the box removes what was built on it.
    document.removeBody(box);
    CHECK(document.bodies().empty());
    CHECK(document.features().empty());
    document.undo();
    CHECK(document.features().size() == 2);
    CHECK(document.featureError(fillet).empty());
}

TEST_CASE("Revolve and shell are features too", "[document][feature]") {
    Document document;
    const ObjectId sketch = document.addSketch("Sketch 1", lcad::kernel::planeXZ());
    lcad::sketch::Sketch s;
    lcad::sketch::addRectangle(s, {20.0, 0.0}, {30.0, 6.0});
    const lcad::sketch::EntityId axis = s.addLine({0.0, -10.0}, {0.0, 30.0});
    s.setConstruction(axis, true);
    document.setSketchGeometry(sketch, s, "Draw");

    Feature turn;
    turn.kind = FeatureKind::Revolve;
    turn.sketch = sketch;
    turn.axis = axis;
    turn.angle = 360.0;
    const ObjectId ring = document.addFeature(turn);
    CHECK(document.featureError(ring).empty());
    CHECK(document.findFeature(ring)->name == "Revolve 1");
    const ObjectId body = document.findFeature(ring)->body;
    CHECK_THAT(lcad::kernel::volume(document.findBody(body)->shape),
               WithinRel(60.0 * 2.0 * pi * 25.0, 1e-9));
    turn = *document.findFeature(ring);
    turn.angle = 90.0;
    document.changeFeature(ring, turn);
    CHECK_THAT(lcad::kernel::volume(document.findBody(body)->shape),
               WithinRel(60.0 * 2.0 * pi * 25.0 / 4.0, 1e-9));
    // Without its axis it cannot be applied, and says what is missing.
    lcad::sketch::Sketch without = document.findSketch(sketch)->geometry;
    without.remove(axis);
    document.setSketchGeometry(sketch, without, "Delete");
    CHECK_THAT(document.featureError(ring), ContainsSubstring("line it turns about"));
    document.undo();
    turn.angle = 400.0;
    document.changeFeature(ring, turn);
    CHECK_THAT(document.featureError(ring), ContainsSubstring("at most 360 degrees"));
    document.undo();

    // Hollow the quarter ring out through one of its ends.
    Feature hollow;
    hollow.kind = FeatureKind::Shell;
    hollow.body = body;
    hollow.size = 1.0;
    for (const std::string& name : document.findBody(body)->faceNames) {
        if (name.find("/start/") != std::string::npos) {
            hollow.faces.push_back(name);
        }
    }
    REQUIRE(hollow.faces.size() == 1);
    const double solid = lcad::kernel::volume(document.findBody(body)->shape);
    const ObjectId shell = document.addFeature(hollow);
    CHECK(document.featureError(shell).empty());
    CHECK(document.findFeature(shell)->name == "Shell 1");
    CHECK(lcad::kernel::volume(document.findBody(body)->shape) < solid * 0.6);
    hollow = *document.findFeature(shell);
    hollow.size = 20.0;
    document.changeFeature(shell, hollow);
    CHECK_THAT(document.featureError(shell), ContainsSubstring("walls are too thick"));
}

TEST_CASE("Saved state with features is checked like the rest", "[document][feature]") {
    Bracket made;
    makeBracket(made);
    const Document& document = made.document;
    const auto stateOf = [&document] {
        DocumentState state;
        state.bodies = document.bodies();
        state.sketches = document.sketches();
        state.features = document.features();
        state.timeline = document.timeline();
        state.nextId = document.nextObjectId();
        return state;
    };

    // Rebuilt from its sketches and features alone, it is the same bracket.
    Document copy(stateOf());
    CHECK(copy.timeline() == document.timeline());
    CHECK(copy.featureError(made.rounds).empty());
    REQUIRE(copy.bodies().size() == 1);
    CHECK_THAT(lcad::kernel::volume(copy.bodies()[0].shape),
               WithinRel(bracketVolume(60.0, 40.0), 1e-9));
    CHECK_FALSE(copy.isModified());
    CHECK_FALSE(copy.canUndo());

    // Without a timeline, things are applied in the order they are listed.
    DocumentState state = stateOf();
    state.timeline.clear();
    CHECK(Document(state).timeline().size() == 5);

    state = stateOf();
    state.timeline.pop_back();
    CHECK_THROWS_AS(Document(state), std::invalid_argument);
    state = stateOf();
    std::swap(state.timeline[0], state.timeline[1]);
    CHECK_THROWS_AS(Document(state), std::invalid_argument);
    state = stateOf();
    state.features[0].body = ObjectId{999};
    CHECK_THROWS_AS(Document(state), std::invalid_argument);
    state = stateOf();
    state.bodies[0].madeBy = made.rounds;
    CHECK_THROWS_AS(Document(state), std::invalid_argument);
    state = stateOf();
    state.features[1].id = state.features[0].id;
    CHECK_THROWS_AS(Document(state), std::invalid_argument);
    state = stateOf();
    state.features[0].name = " ";
    CHECK_THROWS_AS(Document(state), std::invalid_argument);
}

TEST_CASE("A feature can be tried on the model without changing it", "[document][feature]") {
    Bracket made;
    drawOutline(made, 60.0, 40.0);
    Document& document = made.document;
    int heard = 0;
    document.addListener([&heard](const Change&) { ++heard; });
    const std::string drawn = document.undoLabel();

    // A new body: the result is the pulled plate, and the tool is the same solid.
    FeatureTrial trial = document.tryFeature(pullOf(made.outline, 8.0));
    CHECK(trial.error.empty());
    CHECK_THAT(lcad::kernel::volume(trial.result.shape), WithinRel(60.0 * 40.0 * 8.0, 1e-9));
    CHECK_THAT(lcad::kernel::volume(trial.tool.shape), WithinRel(60.0 * 40.0 * 8.0, 1e-9));
    CHECK(trial.result.faceNames.size() == 6);
    CHECK(heard == 0);
    CHECK(document.undoLabel() == drawn);
    CHECK(document.bodies().empty());

    made.plate = document.addFeature(pullOf(made.outline, 8.0));
    const ObjectId body = document.findFeature(made.plate)->body;
    made.holes = document.addSketch("Holes", lcad::kernel::planeXY());
    lcad::sketch::Sketch s;
    made.circle = s.addCircle({20.0, 20.0}, 6.0);
    document.setSketchGeometry(made.holes, s, "Circle");
    const std::string lastStep = document.undoLabel();

    // A cut: the result is the body with the hole, the tool the cylinder that is cut away.
    trial = document.tryFeature(pullOf(made.holes, 8.0, Operation::Cut, body));
    CHECK(trial.error.empty());
    CHECK_THAT(lcad::kernel::volume(trial.result.shape),
               WithinRel((60.0 * 40.0 - pi * 36.0) * 8.0, 1e-6));
    CHECK_THAT(lcad::kernel::volume(trial.tool.shape), WithinRel(pi * 36.0 * 8.0, 1e-6));
    // The body itself is as it was.
    CHECK_THAT(made.volume(), WithinRel(60.0 * 40.0 * 8.0, 1e-9));
    CHECK(document.undoLabel() == lastStep);

    // One that misses the body fails with the reason, and still has its tool to show.
    trial = document.tryFeature(pullOf(made.holes, -8.0, Operation::Cut, body));
    CHECK(trial.result.shape.isNull());
    CHECK_THAT(trial.error, ContainsSubstring("does not reach"));
    CHECK_FALSE(trial.tool.shape.isNull());

    // A fillet of edges the body has, and one whose radius cannot fit.
    Feature rounds;
    rounds.kind = FeatureKind::Fillet;
    rounds.body = body;
    rounds.edges = uprightEdges(document, body);
    rounds.size = 5.0;
    trial = document.tryFeature(rounds);
    CHECK(trial.error.empty());
    CHECK(trial.tool.shape.isNull());
    CHECK_THAT(lcad::kernel::volume(trial.result.shape),
               WithinRel((60.0 * 40.0 - (4.0 - pi) * 25.0) * 8.0, 1e-6));
    rounds.size = 30.0;
    trial = document.tryFeature(rounds);
    CHECK(trial.result.shape.isNull());
    CHECK_FALSE(trial.error.empty());

    // While a feature is being edited, a trial sees the model as it is just before it.
    made.hole = document.addFeature(pullOf(made.holes, 8.0, Operation::Cut, body));
    document.setEditedFeature(made.hole);
    trial = document.tryFeature(pullOf(made.holes, 4.0, Operation::Cut, body));
    CHECK_THAT(lcad::kernel::volume(trial.result.shape),
               WithinRel((60.0 * 40.0 * 8.0 - pi * 36.0 * 4.0), 1e-6));
    document.setEditedFeature({});
    CHECK(heard > 0);
}

TEST_CASE("Patterns and mirrors repeat features and follow them", "[document][feature]") {
    Bracket made;
    drawOutline(made, 60.0, 40.0);
    Document& document = made.document;
    made.plate = document.addFeature(pullOf(made.outline, 8.0));
    const ObjectId body = document.findFeature(made.plate)->body;
    made.holes = document.addSketch("Holes", lcad::kernel::planeXY());
    lcad::sketch::Sketch s;
    made.circle = s.addCircle({10.0, 10.0}, 3.0);
    document.setSketchGeometry(made.holes, s, "Circle");
    made.hole = document.addFeature(pullOf(made.holes, 8.0, Operation::Cut, body));
    const double hole = pi * 9.0 * 8.0;
    const double solid = 60.0 * 40.0 * 8.0;
    REQUIRE_THAT(made.volume(), WithinRel(solid - hole, 1e-6));

    // Four along X, 15 apart, and two along Y, 20 apart: eight holes.
    Feature grid;
    grid.kind = FeatureKind::RectangularPattern;
    grid.body = body;
    grid.sources = {made.hole};
    grid.reference = "X";
    grid.count = 4;
    grid.spacing = 15.0;
    grid.reference2 = "Y";
    grid.count2 = 2;
    grid.spacing2 = 20.0;
    const ObjectId pattern = document.addFeature(grid);
    CHECK(document.findFeature(pattern)->name == "Rectangular Pattern 1");
    CHECK(document.featureError(pattern).empty());
    CHECK_THAT(made.volume(), WithinRel(solid - 8.0 * hole, 1e-6));
    CHECK(document.undoLabel() == "Rectangular Pattern");

    // The copies follow the hole: a larger circle makes every hole larger.
    lcad::sketch::Sketch larger = document.findSketch(made.holes)->geometry;
    larger.addConstraint(ConstraintKind::Radius, made.circle, 0, 0, 4.0);
    larger.solve();
    document.setSketchGeometry(made.holes, larger, "Radius");
    CHECK_THAT(made.volume(), WithinRel(solid - 8.0 * pi * 16.0 * 8.0, 1e-6));
    document.undo();
    CHECK_THAT(made.volume(), WithinRel(solid - 8.0 * hole, 1e-6));

    // Copies that run off the plate take nothing away, and are no failure.
    Feature longer = *document.findFeature(pattern);
    longer.count = 6;
    document.changeFeature(pattern, longer);
    CHECK(document.featureError(pattern).empty());
    CHECK_THAT(made.volume(), WithinRel(solid - 8.0 * hole, 1e-6));
    document.undo();

    // The faces of the copies are named after what they copy, by the pattern.
    bool named = false;
    for (const std::string& face : document.namedBody(body).faceNames) {
        if (face.rfind(std::to_string(pattern.value) + "/1.", 0) == 0 &&
            face.find("/" + std::to_string(made.hole.value) + "/side/") != std::string::npos) {
            named = true;
        }
    }
    CHECK(named);

    // It cannot come before the hole it copies.
    CHECK_THROWS_WITH(document.moveInTimeline(pattern, 2),
                      ContainsSubstring("copies Pull 2, so it cannot come before it"));

    // Suppressing the hole makes the pattern fail, and say why.
    document.setFeatureSuppressed(made.hole, true);
    CHECK_THAT(document.featureError(pattern), ContainsSubstring("is suppressed"));
    CHECK_THAT(made.volume(), WithinRel(solid, 1e-9));
    document.undo();

    // Too few is refused.
    Feature single = *document.findFeature(pattern);
    single.count = 1;
    single.reference2.clear();
    document.changeFeature(pattern, single);
    CHECK_THAT(document.featureError(pattern), ContainsSubstring("nothing to repeat"));
    document.undo();

    // Saved and opened again, the pattern is there and builds the same.
    const lcad::test::TempDir directory;
    const auto path = directory.file("pattern.lcad");
    saveDocument(document, path);
    const LoadedDocument loaded = loadDocument(path);
    CHECK(loaded.warnings.empty());
    const Feature* reread = loaded.document->findFeature(pattern);
    REQUIRE(reread != nullptr);
    CHECK(*reread == *document.findFeature(pattern));
    CHECK_THAT(lcad::kernel::volume(loaded.document->findBody(body)->shape),
               WithinRel(solid - 8.0 * hole, 1e-6));
}

TEST_CASE("Circular patterns and mirrors", "[document][feature]") {
    Bracket made;
    drawOutline(made, 60.0, 40.0);
    Document& document = made.document;
    made.plate = document.addFeature(pullOf(made.outline, 8.0));
    const ObjectId body = document.findFeature(made.plate)->body;
    made.holes = document.addSketch("Holes", lcad::kernel::planeXY());
    lcad::sketch::Sketch s;
    made.circle = s.addCircle({40.0, 20.0}, 2.0);
    document.setSketchGeometry(made.holes, s, "Circle");
    made.hole = document.addFeature(pullOf(made.holes, 8.0, Operation::Cut, body));
    const double hole = pi * 4.0 * 8.0;
    const double solid = 60.0 * 40.0 * 8.0;

    // Around an upright axis through the middle of the plate: the round face of a hole there.
    lcad::sketch::Sketch middle = document.findSketch(made.holes)->geometry;
    middle.addCircle({30.0, 20.0}, 1.0);
    document.setSketchGeometry(made.holes, middle, "Circle");
    REQUIRE(document.featureError(made.hole).empty());
    std::string axisFace;
    const lcad::kernel::NamedShape plate = document.namedBody(body);
    for (std::size_t i = 0; i < plate.faceNames.size(); ++i) {
        const auto axis = lcad::kernel::axisOf(lcad::kernel::subShape(
            plate.shape, lcad::kernel::SubShapeKind::Face, static_cast<int>(i)));
        if (axis.has_value() && std::abs(axis->origin.x - 30.0) < 1e-6 &&
            std::abs(axis->origin.y - 20.0) < 1e-6 && std::abs(axis->direction.z) > 0.999 &&
            plate.faceNames[i].find("/side/") != std::string::npos) {
            axisFace = plate.faceNames[i];
        }
    }
    REQUIRE_FALSE(axisFace.empty());
    // Only the outer hole is to be repeated; the middle one is a hole of its own.
    Feature ring;
    ring.kind = FeatureKind::CircularPattern;
    ring.body = body;
    ring.sources = {made.hole};
    ring.reference = axisFace;
    ring.count = 4;
    ring.angle = 360.0;
    const ObjectId around = document.addFeature(ring);
    CHECK(document.featureError(around).empty());
    // Each copy of the feature copies both of its holes; the middle ones land on each other.
    CHECK_THAT(made.volume(), WithinRel(solid - 4.0 * hole - pi * 8.0, 1e-6));
    const auto box = lcad::kernel::boundingBox(made.body().shape);
    REQUIRE(box.has_value());

    // Half a turn, with three: at 0, 90, and 180 degrees.
    Feature half = *document.findFeature(around);
    half.angle = 180.0;
    half.count = 3;
    document.changeFeature(around, half);
    CHECK_THAT(made.volume(), WithinRel(solid - 3.0 * hole - pi * 8.0, 1e-6));
    document.undo();

    // The face that gives the axis goes: the pattern says so.
    lcad::sketch::Sketch without = document.findSketch(made.holes)->geometry;
    for (const lcad::sketch::Entity& entity : without.entities()) {
        if (entity.kind == lcad::sketch::EntityKind::Circle && entity.id != made.circle) {
            without.remove(entity.id);
            break;
        }
    }
    document.setSketchGeometry(made.holes, without, "Delete");
    CHECK_THAT(document.featureError(around), ContainsSubstring("no longer exists"));
    document.undo();
    CHECK(document.featureError(around).empty());

    // A mirror of the whole body in the YZ plane doubles it.
    Feature mirror;
    mirror.kind = FeatureKind::Mirror;
    mirror.body = body;
    mirror.reference = "YZ";
    const ObjectId image = document.addFeature(mirror);
    CHECK(document.featureError(image).empty());
    CHECK_THAT(made.volume(), WithinRel(2.0 * (solid - 4.0 * hole - pi * 8.0), 1e-6));
    const auto doubled = lcad::kernel::boundingBox(made.body().shape);
    REQUIRE(doubled.has_value());
    CHECK_THAT(doubled->min.x, WithinAbs(-60.0, 1e-6));
    // In a face of the body: the right end, at x = 60.
    std::string end;
    const lcad::kernel::NamedShape now = document.namedBody(body);
    for (std::size_t i = 0; i < now.faceNames.size(); ++i) {
        const auto plane = lcad::kernel::planeOfFace(lcad::kernel::subShape(
            now.shape, lcad::kernel::SubShapeKind::Face, static_cast<int>(i)));
        if (plane.has_value() && plane->normal.x > 0.999) {
            end = now.faceNames[i];
        }
    }
    document.undo();
    REQUIRE_FALSE(end.empty());
    mirror.reference = end;
    const ObjectId other = document.addFeature(mirror);
    CHECK(document.featureError(other).empty());
    const auto across = lcad::kernel::boundingBox(made.body().shape);
    REQUIRE(across.has_value());
    CHECK_THAT(across->max.x, WithinAbs(120.0, 1e-6));
    // A face that is not flat is no plane to mirror in.
    Feature curved = *document.findFeature(other);
    curved.reference = axisFace;
    document.changeFeature(other, curved);
    CHECK_THAT(document.featureError(other), ContainsSubstring("is not flat"));
}

namespace {

// The name of the plate's top face: the flat one facing up at the top.
std::string topFace(const Document& document, ObjectId body) {
    const lcad::kernel::NamedShape named = document.namedBody(body);
    std::string found;
    double highest = -1e9;
    for (std::size_t i = 0; i < named.faceNames.size(); ++i) {
        const lcad::kernel::Shape face = lcad::kernel::subShape(
            named.shape, lcad::kernel::SubShapeKind::Face, static_cast<int>(i));
        const auto plane = lcad::kernel::planeOfFace(face);
        if (plane.has_value() && plane->normal.z > 0.999 && plane->origin.z > highest) {
            highest = plane->origin.z;
            found = named.faceNames[i];
        }
    }
    return found;
}

} // namespace

TEST_CASE("Sketches follow the face or the plane they are drawn on", "[document][feature]") {
    Bracket made;
    drawOutline(made, 60.0, 40.0);
    Document& document = made.document;
    made.plate = document.addFeature(pullOf(made.outline, 8.0));
    const ObjectId body = document.findFeature(made.plate)->body;

    // A hole drawn on the plate's top face, cut 8 down from it.
    const std::string top = topFace(document, body);
    REQUIRE_FALSE(top.empty());
    const lcad::kernel::Shape topShape =
        lcad::kernel::subShape(document.namedBody(body).shape, lcad::kernel::SubShapeKind::Face,
                               lcad::kernel::findFace(document.namedBody(body), top));
    const ObjectId onTop =
        document.addSketch("On top", *lcad::kernel::planeOfFace(topShape), body, top);
    CHECK(document.findSketch(onTop)->attachedTo == body);
    lcad::sketch::Sketch s;
    s.addCircle({10.0, 10.0}, 4.0);
    document.setSketchGeometry(onTop, s, "Circle");
    const ObjectId hole = document.addFeature(pullOf(onTop, -8.0, Operation::Cut, body));
    REQUIRE(document.featureError(hole).empty());
    CHECK_THAT(made.volume(), WithinRel(60.0 * 40.0 * 8.0 - pi * 16.0 * 8.0, 1e-6));

    // The plate gets thicker: the sketch rises with its top, and the hole is 8 deep from there.
    Feature thicker = *document.findFeature(made.plate);
    thicker.distance = 12.0;
    document.changeFeature(made.plate, thicker);
    CHECK_THAT(document.findSketch(onTop)->plane.origin.z, WithinAbs(12.0, 1e-9));
    CHECK(document.featureError(onTop).empty());
    CHECK_THAT(made.volume(), WithinRel(60.0 * 40.0 * 12.0 - pi * 16.0 * 8.0, 1e-6));
    document.undo();
    CHECK_THAT(document.findSketch(onTop)->plane.origin.z, WithinAbs(8.0, 1e-9));

    // It cannot come before the pull that makes the face it is on.
    CHECK_THROWS_WITH(document.moveInTimeline(onTop, 0),
                      ContainsSubstring("is drawn on a face of Body, which Pull 1 makes"));

    // With the plate suppressed, the face is not there: the sketch stays, and says why.
    document.setFeatureSuppressed(made.plate, true);
    CHECK_THAT(document.featureError(onTop), ContainsSubstring("does not exist at this point"));
    CHECK_THAT(document.findSketch(onTop)->plane.origin.z, WithinAbs(8.0, 1e-9));
    document.undo();
    CHECK(document.featureError(onTop).empty());

    // A construction plane 20 above the XY plane, and a disc pulled up from a sketch on it.
    Feature raised;
    raised.kind = FeatureKind::Plane;
    raised.reference = "XY";
    raised.distance = 20.0;
    const ObjectId plane = document.addFeature(raised);
    CHECK(document.findFeature(plane)->name == "Plane 1");
    CHECK(document.featureError(plane).empty());
    REQUIRE(document.planeState(plane).has_value());
    CHECK_THAT(document.planeState(plane)->plane.origin.z, WithinAbs(20.0, 1e-9));
    CHECK(document.bodies().size() == 1);
    const ObjectId onPlane =
        document.addSketch("On the plane", document.planeState(plane)->plane, plane);
    lcad::sketch::Sketch disc;
    disc.addCircle({30.0, 20.0}, 5.0);
    document.setSketchGeometry(onPlane, disc, "Circle");
    const ObjectId lid = document.addFeature(pullOf(onPlane, 5.0));
    const ObjectId lidBody = document.findFeature(lid)->body;
    auto box = lcad::kernel::boundingBox(document.findBody(lidBody)->shape);
    REQUIRE(box.has_value());
    CHECK_THAT(box->min.z, WithinAbs(20.0, 1e-6));

    // Moving the plane moves the sketch and the disc.
    Feature higher = *document.findFeature(plane);
    higher.distance = 30.0;
    document.changeFeature(plane, higher);
    box = lcad::kernel::boundingBox(document.findBody(lidBody)->shape);
    CHECK_THAT(box->min.z, WithinAbs(30.0, 1e-6));
    CHECK_THAT(document.findSketch(onPlane)->plane.origin.z, WithinAbs(30.0, 1e-9));

    // A plane on a face of the body, 10 above the plate's top.
    Feature onFace;
    onFace.kind = FeatureKind::Plane;
    onFace.body = body;
    onFace.reference = top;
    onFace.distance = 10.0;
    const ObjectId above = document.addFeature(onFace);
    REQUIRE(document.planeState(above).has_value());
    CHECK_THAT(document.planeState(above)->plane.origin.z, WithinAbs(18.0, 1e-9));
    // Drawn over the face's middle.
    CHECK_THAT(document.planeState(above)->center.x, WithinAbs(30.0, 1e-6));
    CHECK_THAT(document.planeState(above)->center.y, WithinAbs(20.0, 1e-6));

    // Saved and opened again, all of it is where it was.
    const lcad::test::TempDir directory;
    const auto path = directory.file("planes.lcad");
    saveDocument(document, path);
    const LoadedDocument loaded = loadDocument(path);
    CHECK(loaded.warnings.empty());
    CHECK(loaded.document->findSketch(onTop)->attachedTo == body);
    CHECK(loaded.document->findSketch(onTop)->attachedFace == top);
    CHECK(loaded.document->findSketch(onPlane)->attachedTo == plane);
    CHECK(*loaded.document->findFeature(plane) == *document.findFeature(plane));
    CHECK_THAT(lcad::kernel::volume(loaded.document->findBody(body)->shape),
               WithinRel(made.volume(), 1e-9));
    box = lcad::kernel::boundingBox(loaded.document->findBody(lidBody)->shape);
    CHECK_THAT(box->min.z, WithinAbs(30.0, 1e-6));

    // Attaching to something that is not there is refused.
    CHECK_THROWS_AS(document.addSketch("Nowhere", lcad::kernel::planeXY(), ObjectId{9999}),
                    std::invalid_argument);
    CHECK_THROWS_AS(document.addSketch("No face", lcad::kernel::planeXY(), body),
                    std::invalid_argument);
}

TEST_CASE("Holes are drilled at the points of a sketch", "[document][feature]") {
    Bracket made;
    drawOutline(made, 60.0, 40.0);
    Document& document = made.document;
    made.plate = document.addFeature(pullOf(made.outline, 8.0));
    const ObjectId body = document.findFeature(made.plate)->body;
    const double solid = 60.0 * 40.0 * 8.0;

    // Two points on the plate's top face.
    const std::string top = topFace(document, body);
    const lcad::kernel::NamedShape named = document.namedBody(body);
    const auto plane = lcad::kernel::planeOfFace(lcad::kernel::subShape(
        named.shape, lcad::kernel::SubShapeKind::Face, lcad::kernel::findFace(named, top)));
    const ObjectId marks = document.addSketch("Marks", *plane, body, top);
    lcad::sketch::Sketch s;
    s.addPoint({10.0, 10.0});
    s.addPoint({50.0, 30.0});
    document.setSketchGeometry(marks, s, "Points");

    // 6 across and 5 deep, with a drill's point.
    Feature hole;
    hole.kind = FeatureKind::Hole;
    hole.sketch = marks;
    hole.body = body;
    hole.size = 6.0;
    hole.distance = 5.0;
    const ObjectId holes = document.addFeature(hole);
    CHECK(document.findFeature(holes)->name == "Hole 1");
    REQUIRE(document.featureError(holes).empty());
    const double cone = pi * 9.0 * (3.0 / std::tan(59.0 * pi / 180.0)) / 3.0;
    CHECK_THAT(made.volume(), WithinRel(solid - 2.0 * (pi * 9.0 * 5.0 + cone), 1e-6));

    // Through all.
    hole = *document.findFeature(holes);
    hole.throughAll = true;
    document.changeFeature(holes, hole);
    CHECK_THAT(made.volume(), WithinRel(solid - 2.0 * pi * 9.0 * 8.0, 1e-6));

    // Counterbored, 10 across for the first 3.
    hole.holeKind = HoleKind::Counterbore;
    hole.headDiameter = 10.0;
    hole.headDepth = 3.0;
    document.changeFeature(holes, hole);
    CHECK_THAT(made.volume(), WithinRel(solid - 2.0 * (pi * 25.0 * 3.0 + pi * 9.0 * 5.0), 1e-6));
    // A counterbore narrower than the hole is no counterbore.
    hole.headDiameter = 4.0;
    document.changeFeature(holes, hole);
    CHECK_THAT(document.featureError(holes), ContainsSubstring("wider than the hole"));
    document.undo();

    // A thicker plate: the sketch rises with the top, and the holes still go through.
    Feature thicker = *document.findFeature(made.plate);
    thicker.distance = 12.0;
    document.changeFeature(made.plate, thicker);
    CHECK(document.featureError(holes).empty());
    CHECK_THAT(made.volume(),
               WithinRel(60.0 * 40.0 * 12.0 - 2.0 * (pi * 25.0 * 3.0 + pi * 9.0 * 9.0), 1e-6));
    document.undo();

    // A third point makes a third hole; with no points there is nothing to drill.
    lcad::sketch::Sketch three = document.findSketch(marks)->geometry;
    three.addPoint({30.0, 20.0});
    document.setSketchGeometry(marks, three, "Point");
    CHECK_THAT(made.volume(), WithinRel(solid - 3.0 * (pi * 25.0 * 3.0 + pi * 9.0 * 5.0), 1e-6));
    document.setSketchGeometry(marks, lcad::sketch::Sketch(), "Clear");
    CHECK_THAT(document.featureError(holes), ContainsSubstring("has no points to drill at"));
    CHECK_THAT(made.volume(), WithinRel(solid, 1e-9));
    document.undo();
    document.undo();

    // From a sketch under the plate, the holes go the wrong way until flipped.
    const ObjectId under = document.addSketch("Under", lcad::kernel::planeXY());
    lcad::sketch::Sketch below;
    below.addPoint({30.0, 20.0});
    document.setSketchGeometry(under, below, "Point");
    Feature upward;
    upward.kind = FeatureKind::Hole;
    upward.sketch = under;
    upward.body = body;
    upward.size = 4.0;
    upward.throughAll = true;
    const ObjectId fromBelow = document.addFeature(upward);
    CHECK_THAT(document.featureError(fromBelow), ContainsSubstring("Flip them"));
    upward = *document.findFeature(fromBelow);
    upward.flipped = true;
    document.changeFeature(fromBelow, upward);
    CHECK(document.featureError(fromBelow).empty());

    // Saved and opened again, the holes are the same.
    const double volume = made.volume();
    const lcad::test::TempDir directory;
    const auto path = directory.file("holes.lcad");
    saveDocument(document, path);
    const LoadedDocument loaded = loadDocument(path);
    CHECK(loaded.warnings.empty());
    CHECK(*loaded.document->findFeature(holes) == *document.findFeature(holes));
    CHECK(*loaded.document->findFeature(fromBelow) == *document.findFeature(fromBelow));
    CHECK_THAT(lcad::kernel::volume(loaded.document->findBody(body)->shape),
               WithinRel(volume, 1e-9));
}

TEST_CASE("A pull can taper, or go up to a face and follow it", "[document][feature]") {
    Bracket made;
    drawOutline(made, 60.0, 40.0);
    Document& document = made.document;

    // The plate, its sides leaning in 5 degrees: a prismatoid, whose middle section is the
    // outline shrunk by half as much.
    Feature tapered = pullOf(made.outline, 8.0);
    tapered.taper = 5.0;
    made.plate = document.addFeature(tapered);
    REQUIRE(document.featureError(made.plate).empty());
    const double s = 8.0 * std::tan(5.0 * pi / 180.0);
    const auto area = [](double shrink) { return (60.0 - 2.0 * shrink) * (40.0 - 2.0 * shrink); };
    CHECK_THAT(made.volume(),
               WithinRel(8.0 / 6.0 * (area(0.0) + 4.0 * area(s / 2.0) + area(s)), 1e-6));
    // Both ways at once, with a taper, is refused.
    tapered = *document.findFeature(made.plate);
    tapered.symmetric = true;
    document.changeFeature(made.plate, tapered);
    CHECK_THAT(document.featureError(made.plate), ContainsSubstring("needs a pull to one side"));
    document.undo();
    tapered = *document.findFeature(made.plate);
    tapered.taper = 0.0;
    document.changeFeature(made.plate, tapered);
    CHECK_THAT(made.volume(), WithinRel(60.0 * 40.0 * 8.0, 1e-9));
    const ObjectId body = document.findFeature(made.plate)->body;

    // A post beside the plate, pulled up to the plate's top.
    const ObjectId post = document.addSketch("Post", lcad::kernel::planeXY());
    lcad::sketch::Sketch round;
    round.addCircle({100.0, 0.0}, 5.0);
    document.setSketchGeometry(post, round, "Circle");
    Feature upTo = pullOf(post, 1.0);
    upTo.extentBody = body;
    upTo.extentFace = topFace(document, body);
    const ObjectId rise = document.addFeature(upTo);
    REQUIRE(document.featureError(rise).empty());
    const ObjectId postBody = document.findFeature(rise)->body;
    CHECK_THAT(lcad::kernel::volume(document.findBody(postBody)->shape),
               WithinRel(pi * 25.0 * 8.0, 1e-6));

    // The plate gets thicker; the post follows its top.
    Feature thicker = *document.findFeature(made.plate);
    thicker.distance = 12.0;
    document.changeFeature(made.plate, thicker);
    CHECK_THAT(lcad::kernel::volume(document.findBody(postBody)->shape),
               WithinRel(pi * 25.0 * 12.0, 1e-6));
    // It cannot come before the pull that makes that face. (Its own sketch can.)
    document.moveInTimeline(post, 0);
    CHECK_THROWS_WITH(document.moveInTimeline(rise, 2),
                      ContainsSubstring("goes up to a face of Body, which Pull 1 makes"));
    document.undo();
    // With the plate suppressed, there is no face to go up to.
    document.setFeatureSuppressed(made.plate, true);
    CHECK_THAT(document.featureError(rise), ContainsSubstring("does not exist at this point"));
    document.undo();
    CHECK(document.featureError(rise).empty());

    // Saved and opened again.
    const lcad::test::TempDir directory;
    const auto path = directory.file("taper.lcad");
    saveDocument(document, path);
    const LoadedDocument loaded = loadDocument(path);
    CHECK(loaded.warnings.empty());
    CHECK(*loaded.document->findFeature(rise) == *document.findFeature(rise));
    CHECK_THAT(lcad::kernel::volume(loaded.document->findBody(postBody)->shape),
               WithinRel(pi * 25.0 * 12.0, 1e-6));
}

TEST_CASE("Parameters drive sketch dimensions and feature values", "[document][feature]") {
    Bracket made;
    drawOutline(made, 60.0, 40.0);
    Document& document = made.document;
    std::vector<Change::Kind> heard;
    document.addListener([&heard](const Change& change) { heard.push_back(change.kind); });

    const ObjectId width = document.addParameter("width", "60 mm", "across the plate");
    const ObjectId height = document.addParameter("height", "4 cm");
    const ObjectId thick = document.addParameter("thick", "width / 10");
    CHECK(document.undoLabel() == "Add Parameter");
    CHECK(document.parameterValue(width) == 60.0);
    CHECK(document.parameterValue(height) == 40.0);
    CHECK(document.parameterValue(thick) == 6.0);
    CHECK(std::find(heard.begin(), heard.end(), Change::Kind::ParametersChanged) != heard.end());
    CHECK(document.evaluate("thick * 2 + 1") == 13.0);
    CHECK_THROWS_AS(document.evaluate("depth"), std::invalid_argument);
    CHECK(document.uniqueParameterName("width") == "width1");

    // Names that cannot be, or are taken, are refused.
    CHECK_THROWS_AS(document.addParameter("2wide", "1"), std::invalid_argument);
    CHECK_THROWS_AS(document.addParameter("sqrt", "1"), std::invalid_argument);
    CHECK_THROWS_AS(document.addParameter("width", "1"), std::invalid_argument);

    // The sketch's width and height follow the parameters, and the pull its thickness.
    document.setDimensionExpression(made.outline, made.width, "width");
    document.setDimensionExpression(made.outline, made.height, "height");
    CHECK(document.findSketch(made.outline)->expressions.at(made.width) == "width");
    Feature plate = pullOf(made.outline, 1.0);
    plate.expressions["distance"] = "thick";
    made.plate = document.addFeature(plate);
    REQUIRE(document.featureError(made.plate).empty());
    CHECK_THAT(made.volume(), WithinRel(60.0 * 40.0 * 6.0, 1e-9));
    // The value it holds is what the expression last came to.
    CHECK(document.findFeature(made.plate)->distance == 1.0);

    // One change to the width moves the sketch, and the thickness with it.
    Parameter wider = *document.findParameter(width);
    wider.expression = "85 mm";
    document.changeParameter(width, wider);
    CHECK(document.undoLabel() == "Change Parameter");
    CHECK(document.parameterValue(thick) == 8.5);
    CHECK_THAT(made.volume(), WithinRel(85.0 * 40.0 * 8.5, 1e-9));
    CHECK_THAT(document.findSketch(made.outline)->geometry.findConstraint(made.width)->value,
               WithinRel(85.0, 1e-12));
    document.undo();
    CHECK_THAT(made.volume(), WithinRel(60.0 * 40.0 * 6.0, 1e-9));
    document.redo();
    CHECK_THAT(made.volume(), WithinRel(85.0 * 40.0 * 8.5, 1e-9));

    // A new name goes everywhere the old one was used, in one step.
    Parameter renamed = *document.findParameter(width);
    renamed.name = "span";
    document.changeParameter(width, renamed);
    CHECK(document.findParameter("width") == nullptr);
    CHECK(document.findParameter(thick)->expression == "span / 10");
    CHECK(document.findSketch(made.outline)->expressions.at(made.width) == "span");
    CHECK_THAT(made.volume(), WithinRel(85.0 * 40.0 * 8.5, 1e-9));
    document.undo();
    CHECK(document.findParameter(thick)->expression == "width / 10");
    CHECK(document.findSketch(made.outline)->expressions.at(made.width) == "width");

    // A parameter worked out from itself, through another, cannot be worked out, and says why.
    const ObjectId a = document.addParameter("a", "b + 1");
    const ObjectId b = document.addParameter("b", "a * 2");
    CHECK_FALSE(document.parameterValue(a).has_value());
    CHECK_THAT(document.parameterError(a) + document.parameterError(b),
               ContainsSubstring("which is worked out from it"));
    document.undo();
    document.undo();

    // A feature that uses a parameter that goes fails, and says which.
    document.removeParameter(thick);
    CHECK_THAT(document.featureError(made.plate),
               ContainsSubstring("There is no parameter called thick"));
    document.undo();
    CHECK(document.featureError(made.plate).empty());

    // Saved and opened again, the parameters and expressions are there and work.
    const lcad::test::TempDir directory;
    const auto path = directory.file("parameters.lcad");
    saveDocument(document, path);
    const LoadedDocument loaded = loadDocument(path);
    CHECK(loaded.warnings.empty());
    CHECK(loaded.document->parameters() == document.parameters());
    CHECK(loaded.document->findSketch(made.outline)->expressions ==
          document.findSketch(made.outline)->expressions);
    CHECK(loaded.document->findFeature(made.plate)->expressions ==
          document.findFeature(made.plate)->expressions);
    CHECK_THAT(lcad::kernel::volume(loaded.document->findBody(made.body().id)->shape),
               WithinRel(85.0 * 40.0 * 8.5, 1e-9));

    // A parameter that cannot be worked out is reported on opening.
    Parameter broken = *document.findParameter(height);
    broken.expression = "nothing + 1";
    document.changeParameter(height, broken);
    saveDocument(document, path);
    const LoadedDocument warned = loadDocument(path);
    REQUIRE(!warned.warnings.empty());
    CHECK_THAT(warned.warnings.front(), ContainsSubstring("parameter height cannot be worked out"));
}

TEST_CASE("Ellipses and splines are pulled into solids, and saved", "[document][feature]") {
    Document document;
    const ObjectId sketch = document.addSketch("Curves", lcad::kernel::planeXY());
    lcad::sketch::Sketch s;
    // An ellipse 30 by 12 around (100, 0).
    s.addEllipse({100.0, 0.0}, {130.0, 0.0}, 12.0);
    // A cubic spline closed by a line: 420 square millimeters, worked out by hand.
    const lcad::sketch::EntityId curve =
        s.addSpline({{0.0, 0.0}, {10.0, 20.0}, {30.0, 20.0}, {40.0, 0.0}});
    const lcad::sketch::EntityId base = s.addLine({40.0, 0.0}, {0.0, 0.0});
    s.addConstraint(ConstraintKind::Coincident, s.find(curve)->end, s.find(base)->start);
    s.addConstraint(ConstraintKind::Coincident, s.find(base)->end, s.find(curve)->start);
    REQUIRE(s.solve().status == lcad::sketch::SolveStatus::Solved);
    document.setSketchGeometry(sketch, s, "Curves");
    REQUIRE(document.findSketch(sketch)->geometry.regions().size() == 2);

    const ObjectId pulled = document.addFeature(pullOf(sketch, 5.0));
    REQUIRE(document.featureError(pulled).empty());
    const lcad::kernel::Shape& solid = document.findBody(document.findFeature(pulled)->body)->shape;
    CHECK_THAT(lcad::kernel::volume(solid), WithinRel((pi * 30.0 * 12.0 + 420.0) * 5.0, 1e-9));

    const lcad::test::TempDir directory;
    const auto path = directory.file("curves.lcad");
    saveDocument(document, path);
    const LoadedDocument loaded = loadDocument(path);
    CHECK(loaded.warnings.empty());
    const auto& reread = loaded.document->findSketch(sketch)->geometry;
    CHECK(reread.find(curve)->kind == lcad::sketch::EntityKind::Spline);
    CHECK(reread.find(curve)->points == s.find(curve)->points);
    CHECK_THAT(lcad::kernel::volume(
                   loaded.document->findBody(loaded.document->findFeature(pulled)->body)->shape),
               WithinRel((pi * 30.0 * 12.0 + 420.0) * 5.0, 1e-9));
}
