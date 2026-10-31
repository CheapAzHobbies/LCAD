// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/document/Document.h>
#include <lcad/document/File.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/Plane.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/kernel/Transform.h>
#include <lcad/sketch/Edit.h>

#include <TempDir.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace lcad::document;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using lcad::kernel::Placement;
using lcad::sketch::ConstraintKind;

namespace {

constexpr double tolerance = 1e-6;

// A block made in a component of its own: a dimensioned rectangle on the component's XY plane,
// pulled up.
struct Part {
    ObjectId component;
    ObjectId occurrence;
    ObjectId sketch;
    ObjectId pull;
    ObjectId body;
    lcad::sketch::ConstraintId width = 0;
};

Part makeBlock(Document& document, const std::string& name, double w, double h, double t,
               const Placement& placement = {}) {
    Part part;
    part.component = document.addComponent(name, {}, placement);
    part.occurrence = document.occurrencesOf(part.component).front();
    part.sketch = document.addSketch(document.uniqueSketchName("Sketch"), lcad::kernel::planeXY(),
                                     {}, {}, part.component);
    lcad::sketch::Sketch s;
    const lcad::sketch::RectangleParts r = lcad::sketch::addRectangle(s, {0.0, 0.0}, {w, h});
    s.addConstraint(ConstraintKind::Coincident, s.find(r.bottom)->start, s.origin());
    part.width = s.addConstraint(ConstraintKind::Distance, s.find(r.bottom)->start,
                                 s.find(r.bottom)->end, 0, w);
    s.addConstraint(ConstraintKind::Distance, s.find(r.right)->start, s.find(r.right)->end, 0, h);
    REQUIRE(s.solve().isFullyConstrained());
    document.setSketchGeometry(part.sketch, s, "Rectangle");
    Feature pull;
    pull.kind = FeatureKind::Pull;
    pull.sketch = part.sketch;
    pull.distance = t;
    part.pull = document.addFeature(pull);
    part.body = document.findFeature(part.pull)->body;
    REQUIRE(document.featureError(part.pull).empty());
    return part;
}

void setWidth(Document& document, const Part& part, double value) {
    lcad::sketch::Sketch geometry = document.findSketch(part.sketch)->geometry;
    REQUIRE(lcad::sketch::tryDimensionValue(geometry, part.width, value).ok());
    document.setSketchGeometry(part.sketch, geometry, "Change Dimension");
}

// The name of the face of a body whose frame points along `direction`.
std::string faceFacing(const Document& document, ObjectId body, lcad::kernel::Vector direction) {
    const Body* found = document.findBody(body);
    REQUIRE(found != nullptr);
    for (int i = 0; i < lcad::kernel::subShapeCount(found->shape, lcad::kernel::SubShapeKind::Face);
         ++i) {
        const auto frame = lcad::kernel::frameOn(
            lcad::kernel::subShape(found->shape, lcad::kernel::SubShapeKind::Face, i));
        if (frame.has_value() && std::abs(frame->zAxis.x - direction.x) < 1e-9 &&
            std::abs(frame->zAxis.y - direction.y) < 1e-9 &&
            std::abs(frame->zAxis.z - direction.z) < 1e-9) {
            return found->faceNames[static_cast<std::size_t>(i)];
        }
    }
    FAIL("no face points that way");
    return {};
}

JointSide faceOf(const Document& document, const Part& part, lcad::kernel::Vector direction) {
    JointSide side;
    side.path = {part.occurrence};
    side.kind = JointSide::Kind::Face;
    side.body = part.body;
    side.name = faceFacing(document, part.body, direction);
    return side;
}

// Where a part's body is in the root's coordinates.
lcad::kernel::BoundingBox boundsOf(const Document& document, const Part& part) {
    const lcad::kernel::Shape placed = lcad::kernel::placed(
        document.findBody(part.body)->shape, document.placementOf({part.occurrence}));
    const auto bounds = lcad::kernel::boundingBox(placed);
    REQUIRE(bounds.has_value());
    return *bounds;
}

const lcad::kernel::Vector up{0.0, 0.0, 1.0};
const lcad::kernel::Vector down{0.0, 0.0, -1.0};

Placement somewhereElse() {
    return lcad::kernel::translation({200.0, 50.0, 30.0}) *
           lcad::kernel::rotation(lcad::kernel::Axis{{0.0, 0.0, 0.0}, {1.0, 1.0, 0.0}}, 0.7);
}

} // namespace

TEST_CASE("A component is a part with an occurrence of its own", "[document][assembly]") {
    Document document;
    const ObjectId bracket = document.addComponent("Bracket");

    REQUIRE(document.components().size() == 1);
    REQUIRE(document.occurrencesOf(bracket).size() == 1);
    const ObjectId occurrence = document.occurrencesOf(bracket).front();
    CHECK(document.occurrenceName(occurrence) == "Bracket:1");
    CHECK(document.occurrencesIn({}) == std::vector<ObjectId>{occurrence});
    // The root, and the one occurrence.
    REQUIRE(document.instances().size() == 2);
    CHECK(document.instances()[1].path == std::vector<ObjectId>{occurrence});
    CHECK(document.uniqueComponentName("Component") == "Component 1");

    const ObjectId box = document.addBody("Box", lcad::kernel::makeBox(10.0, 10.0, 10.0), bracket);
    CHECK(document.findBody(box)->component == bracket);
    CHECK(document.componentOf(box) == bracket);

    SECTION("a sketch in it makes bodies in it") {
        const Part block = makeBlock(document, "Block", 20.0, 10.0, 5.0);
        CHECK(document.findBody(block.body)->component == block.component);
        CHECK(document.findFeature(block.pull)->component == block.component);
        CHECK(document.findSketch(block.sketch)->component == block.component);
    }
    SECTION("a feature works within one component") {
        const Part block = makeBlock(document, "Block", 20.0, 10.0, 5.0);
        Feature join;
        join.kind = FeatureKind::Pull;
        join.sketch = block.sketch;
        join.operation = Operation::Join;
        join.body = box;
        CHECK_THROWS_AS(document.addFeature(join), std::invalid_argument);
    }
    SECTION("undo takes the component away, with its occurrence") {
        document.undo(); // the box
        document.undo(); // the component
        CHECK(document.components().empty());
        CHECK(document.occurrences().empty());
        CHECK(document.instances().size() == 1);
        document.redo();
        CHECK(document.components().size() == 1);
        CHECK(document.occurrences().size() == 1);
    }
    SECTION("renaming it renames its occurrences") {
        document.renameComponent(bracket, "Holder");
        CHECK(document.occurrenceName(occurrence) == "Holder:1");
        CHECK_THROWS_AS(document.renameComponent(bracket, " "), std::invalid_argument);
    }
}

TEST_CASE("Occurrences of one component show the same part in several places",
          "[document][assembly]") {
    Document document;
    const Part block = makeBlock(document, "Block", 20.0, 10.0, 5.0);
    const ObjectId second =
        document.addOccurrence(block.component, {}, lcad::kernel::translation({100.0, 0.0, 0.0}));

    CHECK(document.occurrenceName(second) == "Block:2");
    REQUIRE(document.instances().size() == 3);
    CHECK_THAT(boundsOf(document, block).min.x, WithinAbs(0.0, tolerance));
    CHECK_THAT(document.placementOf(second).origin.x, WithinAbs(100.0, tolerance));

    SECTION("changing the part changes both") {
        setWidth(document, block, 30.0);
        const auto bounds = lcad::kernel::boundingBox(document.findBody(block.body)->shape);
        REQUIRE(bounds.has_value());
        CHECK_THAT(bounds->max.x, WithinAbs(30.0, tolerance));
        // Both instances show the one body.
        CHECK(document.instances()[1].component == document.instances()[2].component);
    }
    SECTION("a component cannot go inside itself") {
        CHECK_THROWS_AS(document.addOccurrence(block.component, block.component, {}),
                        std::invalid_argument);
    }
    SECTION("its last occurrence takes the component with it, and undo brings it back") {
        document.removeOccurrence(second);
        CHECK(document.findComponent(block.component) != nullptr);
        document.removeOccurrence(block.occurrence);
        CHECK(document.findComponent(block.component) == nullptr);
        CHECK(document.findBody(block.body) == nullptr);
        CHECK(document.findFeature(block.pull) == nullptr);
        CHECK(document.findSketch(block.sketch) == nullptr);
        CHECK(document.timeline().empty());
        document.undo();
        CHECK(document.findComponent(block.component) != nullptr);
        CHECK(document.findBody(block.body) != nullptr);
        CHECK(!document.findBody(block.body)->shape.isNull());
        CHECK(document.occurrencesOf(block.component).size() == 1);
    }
    SECTION("hidden and suppressed occurrences") {
        document.setOccurrenceVisible(second, false);
        CHECK_FALSE(document.instances()[2].visible);
        document.setOccurrenceSuppressed(second, true);
        CHECK(document.instances().size() == 2);
        document.undo();
        CHECK(document.instances().size() == 3);
    }
}

TEST_CASE("A rigid joint puts one part on another, face to face", "[document][assembly]") {
    Document document;
    const Part base = makeBlock(document, "Base", 40.0, 30.0, 10.0);
    document.setOccurrenceGrounded(base.occurrence, true);
    const Part top = makeBlock(document, "Top", 20.0, 20.0, 5.0, somewhereElse());

    Joint joint;
    joint.kind = JointKind::Rigid;
    joint.moving = faceOf(document, top, down);
    joint.fixed = faceOf(document, base, up);
    const ObjectId id = document.addJoint(joint);

    CHECK(document.findJoint(id)->name == "Rigid 1");
    CHECK(document.jointError(id).empty());
    // The top's underside lies on the base's top, center on center.
    lcad::kernel::BoundingBox bounds = boundsOf(document, top);
    CHECK_THAT(bounds.min.z, WithinAbs(10.0, tolerance));
    CHECK_THAT(bounds.max.z, WithinAbs(15.0, tolerance));
    CHECK_THAT((bounds.min.x + bounds.max.x) / 2.0, WithinAbs(20.0, tolerance));
    CHECK_THAT((bounds.min.y + bounds.max.y) / 2.0, WithinAbs(15.0, tolerance));
    // The base stays where it is.
    CHECK_THAT(boundsOf(document, base).min.z, WithinAbs(0.0, tolerance));

    SECTION("standing off") {
        Joint changed = *document.findJoint(id);
        changed.offset = 2.0;
        document.changeJoint(id, changed);
        CHECK_THAT(boundsOf(document, top).min.z, WithinAbs(12.0, tolerance));
    }
    SECTION("flipped: the faces point the same way") {
        Joint changed = *document.findJoint(id);
        changed.flipped = true;
        document.changeJoint(id, changed);
        bounds = boundsOf(document, top);
        CHECK_THAT(bounds.min.z, WithinAbs(5.0, tolerance));
        CHECK_THAT(bounds.max.z, WithinAbs(10.0, tolerance));
    }
    SECTION("the joint follows the faces when a part changes") {
        Feature thicker = *document.findFeature(base.pull);
        thicker.distance = 25.0;
        document.changeFeature(base.pull, thicker);
        CHECK_THAT(boundsOf(document, top).min.z, WithinAbs(25.0, tolerance));
        setWidth(document, base, 60.0);
        bounds = boundsOf(document, top);
        CHECK_THAT((bounds.min.x + bounds.max.x) / 2.0, WithinAbs(30.0, tolerance));
    }
    SECTION("deleting the joint leaves the part where it is") {
        const Placement before = document.placementOf(top.occurrence);
        document.removeJoint(id);
        CHECK(document.joints().empty());
        CHECK(lcad::kernel::samePlacement(document.placementOf(top.occurrence), before, 1e-9));
        document.undo();
        CHECK(document.joints().size() == 1);
    }
    SECTION("a face that is gone is reported") {
        Joint changed = *document.findJoint(id);
        changed.fixed.name = "nothing/like/this";
        document.changeJoint(id, changed);
        CHECK_THAT(document.jointError(id), ContainsSubstring("no longer exists"));
        // The part goes back to where it was put by hand.
        CHECK(lcad::kernel::samePlacement(document.placementOf(top.occurrence),
                                          document.findOccurrence(top.occurrence)->placement));
    }
    SECTION("deleting the occurrence takes the joint with it") {
        document.removeOccurrence(top.occurrence);
        CHECK(document.joints().empty());
        document.undo();
        CHECK(document.joints().size() == 1);
        CHECK(document.jointError(id).empty());
    }
}

TEST_CASE("A revolute joint turns its part, within its limits", "[document][assembly]") {
    Document document;
    const Part plate = makeBlock(document, "Plate", 60.0, 60.0, 5.0);
    document.setOccurrenceGrounded(plate.occurrence, true);
    const Part arm = makeBlock(document, "Arm", 40.0, 10.0, 5.0, somewhereElse());

    Joint joint;
    joint.kind = JointKind::Revolute;
    joint.moving = faceOf(document, arm, down);
    joint.fixed = faceOf(document, plate, up);
    const ObjectId id = document.addJoint(joint);
    REQUIRE(document.jointError(id).empty());

    const auto extent = [&] {
        const lcad::kernel::BoundingBox b = boundsOf(document, arm);
        return std::pair{b.max.x - b.min.x, b.max.y - b.min.y};
    };
    // At no turn the arm lies along X.
    CHECK_THAT(extent().first, WithinAbs(40.0, tolerance));
    CHECK_THAT(extent().second, WithinAbs(10.0, tolerance));

    Joint turned = *document.findJoint(id);
    turned.values[0] = 90.0;
    document.changeJoint(id, turned);
    CHECK_THAT(extent().first, WithinAbs(10.0, tolerance));
    CHECK_THAT(extent().second, WithinAbs(40.0, tolerance));
    // It turns about the middle of the plate's top.
    const lcad::kernel::BoundingBox b = boundsOf(document, arm);
    CHECK_THAT((b.min.x + b.max.x) / 2.0, WithinAbs(30.0, tolerance));
    CHECK_THAT((b.min.y + b.max.y) / 2.0, WithinAbs(30.0, tolerance));

    SECTION("a value beyond a limit is held at the limit") {
        Joint limited = *document.findJoint(id);
        limited.limits[0].maximum = 45.0;
        document.changeJoint(id, limited);
        CHECK_THAT(document.findJoint(id)->values[0], WithinAbs(45.0, 1e-12));
        const double diagonal = 40.0 * std::cos(3.14159265358979323846 / 4.0) +
                                10.0 * std::sin(3.14159265358979323846 / 4.0);
        CHECK_THAT(extent().first, WithinAbs(diagonal, tolerance));
    }
    SECTION("motion can be shown without changing the document") {
        const std::vector<Instance> moved = document.instancesWith({{id, {0.0, 0.0, 0.0}}});
        const auto found = std::find_if(moved.begin(), moved.end(), [&](const Instance& i) {
            return i.path == std::vector<ObjectId>{arm.occurrence};
        });
        REQUIRE(found != moved.end());
        const auto bounds = lcad::kernel::boundingBox(
            lcad::kernel::placed(document.findBody(arm.body)->shape, found->placement));
        REQUIRE(bounds.has_value());
        CHECK_THAT(bounds->max.x - bounds->min.x, WithinAbs(40.0, tolerance));
        // The document is as it was.
        CHECK_THAT(extent().first, WithinAbs(10.0, tolerance));
    }
    SECTION("a slider slides along the axis") {
        Joint slider = *document.findJoint(id);
        slider.kind = JointKind::Slider;
        slider.values = {7.0, 0.0, 0.0};
        document.changeJoint(id, slider);
        CHECK_THAT(boundsOf(document, arm).min.z, WithinAbs(12.0, tolerance));
    }
    SECTION("a trial changes nothing") {
        Joint tried = *document.findJoint(id);
        tried.values[0] = 0.0;
        const JointTrial trial = document.tryJoint(tried);
        CHECK(trial.error.empty());
        CHECK(trial.instances.size() == document.instances().size());
        CHECK_THAT(document.findJoint(id)->values[0], WithinAbs(90.0, 1e-12));
    }
}

TEST_CASE("A joint moves the other side when the side meant to move cannot",
          "[document][assembly]") {
    Document document;
    const Part base = makeBlock(document, "Base", 40.0, 30.0, 10.0, somewhereElse());
    const Part top = makeBlock(document, "Top", 20.0, 20.0, 5.0);
    document.setOccurrenceGrounded(top.occurrence, true);

    Joint joint;
    joint.moving = faceOf(document, top, down);
    joint.fixed = faceOf(document, base, up);
    const ObjectId id = document.addJoint(joint);
    CHECK(document.jointError(id).empty());
    // The top stays at the origin; the base comes up under it.
    CHECK_THAT(boundsOf(document, top).min.z, WithinAbs(0.0, tolerance));
    const lcad::kernel::BoundingBox bounds = boundsOf(document, base);
    CHECK_THAT(bounds.max.z, WithinAbs(0.0, tolerance));
    CHECK_THAT(bounds.min.z, WithinAbs(-10.0, tolerance));
    CHECK_THAT((bounds.min.x + bounds.max.x) / 2.0, WithinAbs(10.0, tolerance));

    SECTION("with neither grounded, the first side moves to the second") {
        document.setOccurrenceGrounded(top.occurrence, false);
        CHECK(lcad::kernel::samePlacement(document.placementOf(base.occurrence),
                                          document.findOccurrence(base.occurrence)->placement));
        // The base is tilted where it was put; the top's underside lies on its top all the same.
        const auto centerOf = [&](const Part& part, lcad::kernel::Vector direction) {
            const Body* body = document.findBody(part.body);
            const int face = lcad::kernel::findFace(document.namedBody(part.body),
                                                    faceFacing(document, part.body, direction));
            const auto frame = lcad::kernel::frameOn(lcad::kernel::placed(
                lcad::kernel::subShape(body->shape, lcad::kernel::SubShapeKind::Face, face),
                document.placementOf(part.occurrence)));
            REQUIRE(frame.has_value());
            return *frame;
        };
        const Placement under = centerOf(top, down);
        const Placement on = centerOf(base, up);
        CHECK_THAT(under.origin.x, WithinAbs(on.origin.x, tolerance));
        CHECK_THAT(under.origin.y, WithinAbs(on.origin.y, tolerance));
        CHECK_THAT(under.origin.z, WithinAbs(on.origin.z, tolerance));
        CHECK_THAT(under.zAxis.z, WithinAbs(-on.zAxis.z, tolerance));
    }
}

TEST_CASE("Joints that cannot hold together are reported", "[document][assembly]") {
    Document document;
    const Part a = makeBlock(document, "A", 40.0, 40.0, 10.0);
    document.setOccurrenceGrounded(a.occurrence, true);
    const Part b = makeBlock(document, "B", 20.0, 20.0, 5.0, somewhereElse());
    const Part c = makeBlock(document, "C", 10.0, 10.0, 5.0, somewhereElse());

    Joint first;
    first.moving = faceOf(document, b, down);
    first.fixed = faceOf(document, a, up);
    document.addJoint(first);
    Joint second;
    second.moving = faceOf(document, c, down);
    second.fixed = faceOf(document, b, up);
    document.addJoint(second);
    CHECK(document.jointError(document.joints()[1].id).empty());
    CHECK_THAT(boundsOf(document, c).min.z, WithinAbs(15.0, tolerance));

    // C on A as well: it is on B already, five millimeters higher.
    Joint third;
    third.moving = faceOf(document, c, down);
    third.fixed = faceOf(document, a, up);
    const ObjectId loop = document.addJoint(third);
    CHECK_THAT(document.jointError(loop), ContainsSubstring("cannot hold together"));
    // One that fits does hold.
    Joint fits = third;
    fits.offset = 5.0;
    document.changeJoint(loop, fits);
    CHECK(document.jointError(loop).empty());

    SECTION("both sides on one part") {
        Joint same;
        same.moving = faceOf(document, b, down);
        same.fixed = faceOf(document, b, up);
        const ObjectId id = document.addJoint(same);
        CHECK_THAT(document.jointError(id), ContainsSubstring("Both of its sides"));
    }
    SECTION("a side that does not lie in the joint's component is refused") {
        Joint wrong = first;
        wrong.moving.path.clear();
        CHECK_THROWS_AS(document.addJoint(wrong), std::invalid_argument);
    }
    SECTION("a suppressed joint holds nothing") {
        Joint off = *document.findJoint(loop);
        off.suppressed = true;
        document.changeJoint(loop, off);
        CHECK(document.jointError(loop).empty());
    }
}

TEST_CASE("Bodies with their features become a component", "[document][assembly]") {
    Document document;
    const ObjectId sketch = document.addSketch("Sketch 1", lcad::kernel::planeXY());
    lcad::sketch::Sketch s;
    lcad::sketch::addRectangle(s, {0.0, 0.0}, {20.0, 10.0});
    document.setSketchGeometry(sketch, s, "Rectangle");
    Feature pull;
    pull.kind = FeatureKind::Pull;
    pull.sketch = sketch;
    pull.distance = 5.0;
    const ObjectId made = document.addFeature(pull);
    const ObjectId body = document.findFeature(made)->body;
    const double volume = lcad::kernel::volume(document.findBody(body)->shape);

    const ObjectId component = document.makeComponent("Plate", {body});

    CHECK(document.findBody(body)->component == component);
    CHECK(document.findFeature(made)->component == component);
    CHECK(document.findSketch(sketch)->component == component);
    REQUIRE(document.occurrencesOf(component).size() == 1);
    CHECK(lcad::kernel::samePlacement(
        document.placementOf(document.occurrencesOf(component).front()), Placement{}));
    CHECK_THAT(lcad::kernel::volume(document.findBody(body)->shape), WithinRel(volume, 1e-12));
    CHECK(document.undoLabel() == "Make Component");

    SECTION("undo puts them back") {
        document.undo();
        CHECK(document.components().empty());
        CHECK_FALSE(document.findBody(body)->component.isValid());
        CHECK_FALSE(document.findSketch(sketch)->component.isValid());
    }
    SECTION("a sketch shared with a body that stays stands in the way") {
        document.undo();
        const ObjectId again = document.addFeature(pull);
        const ObjectId other = document.findFeature(again)->body;
        CHECK_THROWS_WITH(document.makeComponent("Plate", {body}), ContainsSubstring("Sketch 1"));
        CHECK_NOTHROW(document.makeComponent("Plates", {body, other}));
    }
}

TEST_CASE("A copy of a component is changed on its own", "[document][assembly]") {
    Document document;
    const Part block = makeBlock(document, "Block", 20.0, 10.0, 5.0);
    // A fillet refers to an edge by the names of the faces the pull made.
    const std::vector<std::string> edges = lcad::kernel::edgeNames(document.namedBody(block.body));
    Feature fillet;
    fillet.kind = FeatureKind::Fillet;
    fillet.body = block.body;
    fillet.edges = {edges.front()};
    fillet.size = 1.0;
    const ObjectId rounded = document.addFeature(fillet);
    REQUIRE(document.featureError(rounded).empty());
    const double volume = lcad::kernel::volume(document.findBody(block.body)->shape);

    const ObjectId copy = document.copyComponent(block.occurrence);

    const ObjectId copied = document.findOccurrence(copy)->component;
    REQUIRE(copied != block.component);
    CHECK(document.findComponent(copied)->name == "Block copy");
    // Everything was copied, and the copy's fillet finds its edge.
    std::vector<ObjectId> copiedBodies;
    for (const Body& body : document.bodies()) {
        if (body.component == copied) {
            copiedBodies.push_back(body.id);
        }
    }
    REQUIRE(copiedBodies.size() == 1);
    CHECK_THAT(lcad::kernel::volume(document.findBody(copiedBodies.front())->shape),
               WithinRel(volume, 1e-9));
    for (const Feature& feature : document.features()) {
        CHECK(document.featureError(feature.id).empty());
    }
    CHECK(document.features().size() == 4);

    SECTION("changing the copy leaves the original as it is") {
        ObjectId copiedSketch;
        for (const SketchItem& sketch : document.sketches()) {
            if (sketch.component == copied) {
                copiedSketch = sketch.id;
            }
        }
        REQUIRE(copiedSketch.isValid());
        lcad::sketch::Sketch geometry = document.findSketch(copiedSketch)->geometry;
        REQUIRE(lcad::sketch::tryDimensionValue(geometry, block.width, 40.0).ok());
        document.setSketchGeometry(copiedSketch, geometry, "Change Dimension");
        CHECK_THAT(lcad::kernel::volume(document.findBody(block.body)->shape),
                   WithinRel(volume, 1e-9));
        CHECK(lcad::kernel::volume(document.findBody(copiedBodies.front())->shape) > volume * 1.5);
        for (const Feature& feature : document.features()) {
            CHECK(document.featureError(feature.id).empty());
        }
    }
    SECTION("undo takes the copy away in one step") {
        document.undo();
        CHECK(document.components().size() == 1);
        CHECK(document.features().size() == 2);
    }
}

TEST_CASE("Two parts joined together come back from a file where they were",
          "[document][assembly][file]") {
    // The acceptance test of Phase 5: two independently editable parts, held by a joint, saved,
    // opened again, and still where they were, still editable.
    lcad::test::TempDir directory;
    const auto path = directory.file("assembly.lcad");
    std::vector<ObjectId> armPath;
    lcad::kernel::BoundingBox before{};
    {
        Document document;
        const Part plate = makeBlock(document, "Plate", 60.0, 60.0, 5.0);
        document.setOccurrenceGrounded(plate.occurrence, true);
        const Part arm = makeBlock(document, "Arm", 40.0, 10.0, 5.0, somewhereElse());
        Joint joint;
        joint.kind = JointKind::Revolute;
        joint.moving = faceOf(document, arm, down);
        joint.fixed = faceOf(document, plate, up);
        joint.values[0] = 30.0;
        joint.limits[0].minimum = -90.0;
        joint.limits[0].maximum = 90.0;
        document.addJoint(joint);
        document.setBodyMaterial(arm.body, "Aluminum");
        before = boundsOf(document, arm);
        armPath = {arm.occurrence};
        saveDocument(document, path);
    }

    LoadedDocument loaded = loadDocument(path);
    INFO(loaded.warnings.size());
    CHECK(loaded.warnings.empty());
    Document& document = *loaded.document;
    REQUIRE(document.components().size() == 2);
    REQUIRE(document.joints().size() == 1);
    const Joint& joint = document.joints().front();
    CHECK(joint.kind == JointKind::Revolute);
    CHECK_THAT(joint.values[0], WithinAbs(30.0, 1e-12));
    CHECK(joint.limits[0].maximum == 90.0);
    CHECK_FALSE(joint.limits[1].maximum.has_value());
    CHECK(document.jointError(joint.id).empty());

    Part arm;
    arm.occurrence = armPath.front();
    arm.component = document.findOccurrence(arm.occurrence)->component;
    for (const Body& body : document.bodies()) {
        if (body.component == arm.component) {
            arm.body = body.id;
        }
    }
    CHECK(document.findBody(arm.body)->material == "Aluminum");
    const lcad::kernel::BoundingBox after = boundsOf(document, arm);
    CHECK_THAT(after.min.x, WithinAbs(before.min.x, tolerance));
    CHECK_THAT(after.max.y, WithinAbs(before.max.y, tolerance));
    CHECK_THAT(after.min.z, WithinAbs(before.min.z, tolerance));

    // Each part is still edited on its own, and the joint follows.
    const ObjectId plateComponent = document.findOccurrence(joint.fixed.path.front())->component;
    for (const Feature& feature : document.features()) {
        if (feature.component == plateComponent) {
            Feature thicker = feature;
            thicker.distance = 12.0;
            document.changeFeature(feature.id, thicker);
        }
    }
    CHECK_THAT(boundsOf(document, arm).min.z, WithinAbs(12.0, tolerance));
    CHECK(document.jointError(joint.id).empty());
}

TEST_CASE("A linked component keeps its bodies, and what is joined to them, when it changes",
          "[document][assembly]") {
    Document document;
    const Part base = makeBlock(document, "Base", 40.0, 40.0, 10.0);
    document.setOccurrenceGrounded(base.occurrence, true);
    const auto named = [](const lcad::kernel::Shape& shape) {
        return lcad::kernel::nameFaces(shape, "9/f").faceNames;
    };
    LinkedContent content;
    content.stamp = "first";
    const lcad::kernel::Shape cubeShape = lcad::kernel::makeBox(10.0, 10.0, 10.0);
    content.bodies.push_back(LinkedBody{"9", "Cube", cubeShape, named(cubeShape), "PLA"});

    const ObjectId linked =
        document.addLinkedComponent("Cube", "parts/cube.lcad", content, {}, somewhereElse());

    const Component* component = document.findComponent(linked);
    REQUIRE(component != nullptr);
    CHECK(component->link == "parts/cube.lcad");
    CHECK(component->linkStamp == "first");
    ObjectId cube;
    for (const Body& body : document.bodies()) {
        if (body.component == linked) {
            cube = body.id;
        }
    }
    REQUIRE(cube.isValid());
    CHECK(document.findBody(cube)->material == "PLA");
    // Not part of this document's history, and not changed here.
    CHECK(std::find(document.timeline().begin(), document.timeline().end(), cube) ==
          document.timeline().end());
    Feature cut;
    cut.kind = FeatureKind::Fillet;
    cut.body = cube;
    cut.edges = {"a|b"};
    CHECK_THROWS_AS(document.addFeature(cut), std::invalid_argument);
    CHECK_THROWS_AS(document.addBody("More", cubeShape, linked), std::invalid_argument);

    Part part;
    part.occurrence = document.occurrencesOf(linked).front();
    part.body = cube;
    Joint joint;
    joint.moving = faceOf(document, part, down);
    joint.fixed = faceOf(document, base, up);
    const ObjectId id = document.addJoint(joint);
    CHECK_THAT(boundsOf(document, part).max.z, WithinAbs(20.0, tolerance));

    SECTION("a new version keeps the body and the joint") {
        LinkedContent bigger;
        bigger.stamp = "second";
        const lcad::kernel::Shape tall = lcad::kernel::makeBox(10.0, 10.0, 30.0);
        bigger.bodies.push_back(LinkedBody{"9", "Cube", tall, named(tall), {}});
        document.updateLinkedComponent(linked, bigger);
        CHECK(document.findBody(cube) != nullptr);
        CHECK(document.findComponent(linked)->linkStamp == "second");
        CHECK(document.jointError(id).empty());
        CHECK_THAT(boundsOf(document, part).max.z, WithinAbs(40.0, tolerance));
        // The material it was given stays, as the new version says none.
        CHECK(document.findBody(cube)->material == "PLA");
        document.undo();
        CHECK_THAT(boundsOf(document, part).max.z, WithinAbs(20.0, tolerance));
    }
    SECTION("a body the new version lacks goes, and the joint on it says so") {
        LinkedContent empty;
        empty.stamp = "third";
        const lcad::kernel::Shape other = lcad::kernel::makeSphere(5.0);
        empty.bodies.push_back(LinkedBody{"12", "Ball", other, named(other), {}});
        document.updateLinkedComponent(linked, empty);
        CHECK(document.findBody(cube) == nullptr);
        CHECK_FALSE(document.jointError(id).empty());
    }
    SECTION("saved, its bodies come back from the copy kept in the file") {
        lcad::test::TempDir directory;
        saveDocument(document, directory.file("linked.lcad"));
        const LoadedDocument loaded = loadDocument(directory.file("linked.lcad"));
        const Body* again = loaded.document->findBody(cube);
        REQUIRE(again != nullptr);
        CHECK(again->linkKey == "9");
        CHECK(again->sourceFaceNames == document.findBody(cube)->sourceFaceNames);
        CHECK(loaded.document->findComponent(linked)->link == "parts/cube.lcad");
        CHECK(loaded.document->jointError(id).empty());
    }
}

TEST_CASE("Materials say what bodies are made of", "[document][assembly]") {
    Document document;
    const ObjectId box = document.addBody("Box", lcad::kernel::makeBox(10.0, 10.0, 10.0));
    std::vector<Change::Kind> heard;
    document.addListener([&](const Change& change) { heard.push_back(change.kind); });

    document.setBodyMaterial(box, "Steel");
    CHECK(document.findBody(box)->material == "Steel");
    CHECK(std::find(heard.begin(), heard.end(), Change::Kind::MaterialsChanged) != heard.end());
    CHECK_THAT(document.findMaterial("Steel")->density, WithinAbs(7.85, 1e-12));
    CHECK_THROWS_AS(document.setBodyMaterial(box, "Unobtainium"), std::invalid_argument);

    document.addMaterial({"Carbon nylon", 1.3});
    CHECK_THROWS_AS(document.addMaterial({"Carbon nylon", 1.2}), std::invalid_argument);
    CHECK_THROWS_AS(document.addMaterial({"Foam", 0.0}), std::invalid_argument);
    document.setBodyMaterial(box, "Carbon nylon");
    document.changeMaterial("Carbon nylon", {"CF nylon", 1.25});
    CHECK(document.findBody(box)->material == "CF nylon");
    CHECK_THAT(document.findMaterial("CF nylon")->density, WithinAbs(1.25, 1e-12));

    SECTION("saved with the document") {
        lcad::test::TempDir directory;
        saveDocument(document, directory.file("materials.lcad"));
        const LoadedDocument loaded = loadDocument(directory.file("materials.lcad"));
        REQUIRE(loaded.document->materials().size() == 1);
        CHECK(loaded.document->materials().front() == Material{"CF nylon", 1.25});
        CHECK(loaded.document->findBody(box)->material == "CF nylon");
    }
    SECTION("removed, its bodies are made of nothing said") {
        document.removeMaterial("CF nylon");
        CHECK(document.findBody(box)->material.empty());
        document.undo();
        CHECK(document.findBody(box)->material == "CF nylon");
    }
}

TEST_CASE("An occurrence moved into another component stays where it is", "[document][assembly]") {
    Document document;
    const ObjectId frame =
        document.addComponent("Frame", {}, lcad::kernel::translation({0.0, 0.0, 50.0}));
    const Part wheel =
        makeBlock(document, "Wheel", 10.0, 10.0, 10.0, lcad::kernel::translation({30.0, 0.0, 0.0}));
    const lcad::kernel::BoundingBox before = boundsOf(document, wheel);

    document.moveOccurrence(wheel.occurrence, frame);

    CHECK(document.findOccurrence(wheel.occurrence)->parent == frame);
    const auto inside = std::find_if(
        document.instances().begin(), document.instances().end(),
        [&](const Instance& i) { return !i.path.empty() && i.path.back() == wheel.occurrence; });
    REQUIRE(inside != document.instances().end());
    CHECK(inside->path.size() == 2);
    const auto bounds = lcad::kernel::boundingBox(
        lcad::kernel::placed(document.findBody(wheel.body)->shape, inside->placement));
    REQUIRE(bounds.has_value());
    CHECK_THAT(bounds->min.x, WithinAbs(before.min.x, tolerance));
    CHECK_THAT(bounds->min.z, WithinAbs(before.min.z, tolerance));
    CHECK_THROWS_AS(document.moveOccurrence(document.occurrencesOf(frame).front(), wheel.component),
                    std::invalid_argument);
}
