// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/document/Document.h>
#include <lcad/kernel/Exchange.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/MeshSolid.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/Plane.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/kernel/Transform.h>
#include <lcad/recover/Recover.h>
#include <lcad/sketch/Edit.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace lcad::document;
using namespace lcad::recover;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using lcad::kernel::Mesh;
using lcad::kernel::Plane;
using lcad::kernel::Point;
using lcad::sketch::ConstraintKind;

namespace {

const double pi = 3.14159265358979323846;

// Makes a part the way a person would, for its mesh.
struct Maker {
    Document document;
    ObjectId body;

    ObjectId sketch(const Plane& plane, const lcad::sketch::Sketch& geometry) {
        const ObjectId id = document.addSketch(document.uniqueSketchName("Sketch "), plane);
        document.setSketchGeometry(id, geometry, "Draw");
        return id;
    }
    ObjectId pull(const lcad::sketch::Sketch& geometry, const Plane& plane, double distance,
                  Operation operation = Operation::NewBody) {
        Feature feature;
        feature.kind = FeatureKind::Pull;
        feature.sketch = sketch(plane, geometry);
        feature.distance = distance;
        feature.operation = operation;
        feature.body = body;
        const ObjectId made = document.addFeature(feature);
        REQUIRE(document.featureError(made).empty());
        body = document.findFeature(made)->body;
        return made;
    }
    void hole(const Plane& plane, const std::vector<lcad::sketch::Point2>& points, double diameter,
              double depth) {
        lcad::sketch::Sketch geometry;
        for (const lcad::sketch::Point2 p : points) {
            geometry.addPoint(p);
        }
        Feature feature;
        feature.kind = FeatureKind::Hole;
        feature.sketch = sketch(plane, geometry);
        feature.body = body;
        feature.size = diameter;
        feature.distance = depth;
        feature.tipAngle = 180.0;
        const ObjectId made = document.addFeature(feature);
        REQUIRE(document.featureError(made).empty());
    }
    void blend(FeatureKind kind, double size, const std::vector<Point>& places) {
        const lcad::kernel::NamedShape named = document.namedBody(body);
        const std::vector<std::string> names = lcad::kernel::edgeNames(named);
        Feature feature;
        feature.kind = kind;
        feature.body = body;
        feature.size = size;
        for (const Point& at : places) {
            const int edge = lcad::kernel::nearestEdge(named.shape, at, 0.01);
            REQUIRE(edge >= 0);
            feature.edges.push_back(names[static_cast<std::size_t>(edge)]);
        }
        const ObjectId made = document.addFeature(feature);
        INFO(document.featureError(made));
        REQUIRE(document.featureError(made).empty());
    }
    [[nodiscard]] const lcad::kernel::Shape& shape() const { return document.findBody(body)->shape; }
    [[nodiscard]] Mesh mesh(double deviation = 0.02) const {
        return lcad::kernel::triangulate(shape(), {deviation, 10.0});
    }
};

lcad::sketch::Sketch rectangle(double x0, double y0, double x1, double y1) {
    lcad::sketch::Sketch s;
    lcad::sketch::addRectangle(s, {x0, y0}, {x1, y1});
    return s;
}

Plane at(double height) {
    return lcad::kernel::makePlane({0.0, 0.0, height}, {0.0, 0.0, 1.0}, {1.0, 0.0, 0.0});
}

int countOf(const Document& document, FeatureKind kind) {
    return static_cast<int>(std::count_if(document.features().begin(), document.features().end(),
                                          [kind](const Feature& f) { return f.kind == kind; }));
}

const Feature& firstOf(const Document& document, FeatureKind kind) {
    return *std::find_if(document.features().begin(), document.features().end(),
                         [kind](const Feature& f) { return f.kind == kind; });
}

double volumeOf(const Document& document, const MeshResult& result) {
    double total = 0.0;
    for (const ObjectId body : result.bodies) {
        total += lcad::kernel::volume(document.findBody(body)->shape);
    }
    return total;
}

void show(const char* name, const MeshResult& made) {
    for (const PartResult& part : made.parts) {
        INFO(name << ": result " << static_cast<int>(part.result) << " recognized " << part.recognized
                  << " pulls " << part.pulls << " holes " << part.holes << " fillets " << part.fillets
                  << " chamfers " << part.chamfers << " note: " << part.note);
        CHECK(part.result == Result::Features);
    }
}

} // namespace

TEST_CASE("A box comes back as one pull", "[recover]") {
    Maker part;
    part.pull(rectangle(0.0, 0.0, 40.0, 25.0), at(0.0), 10.0);
    Document model;
    const MeshResult made = addMesh(model, part.mesh(), "Box");
    show("box", made);
    CHECK(countOf(model, FeatureKind::Pull) == 1);
    CHECK_THAT(firstOf(model, FeatureKind::Pull).distance, WithinAbs(10.0, 1e-6));
    CHECK_THAT(volumeOf(model, made), WithinRel(40.0 * 25.0 * 10.0, 1e-6));
    REQUIRE(model.sketches().size() == 1);
    CHECK(model.bodies().front().name == "Box");
}

TEST_CASE("Holes and rounds come back as Hole and Fillet", "[recover]") {
    Maker part;
    part.pull(rectangle(0.0, 0.0, 60.0, 40.0), at(0.0), 8.0);
    part.hole(at(8.0), {{10.0, 10.0}, {50.0, 10.0}, {50.0, 30.0}, {10.0, 30.0}}, 6.0, 8.0);
    part.blend(FeatureKind::Fillet, 5.0,
               {{0.0, 0.0, 4.0}, {60.0, 0.0, 4.0}, {60.0, 40.0, 4.0}, {0.0, 40.0, 4.0}});
    const double wanted = lcad::kernel::volume(part.shape());
    Document model;
    const MeshResult made = addMesh(model, part.mesh(), "Plate");
    show("plate", made);
    CHECK(countOf(model, FeatureKind::Pull) == 1);
    REQUIRE(countOf(model, FeatureKind::Hole) == 1);
    CHECK_THAT(firstOf(model, FeatureKind::Hole).size, WithinAbs(6.0, 1e-3));
    REQUIRE(countOf(model, FeatureKind::Fillet) == 1);
    CHECK_THAT(firstOf(model, FeatureKind::Fillet).size, WithinAbs(5.0, 1e-3));
    CHECK(firstOf(model, FeatureKind::Fillet).edges.size() == 4);
    CHECK_THAT(volumeOf(model, made), WithinRel(wanted, 1e-3));
}

TEST_CASE("A chamfer comes back as a Chamfer", "[recover]") {
    Maker part;
    part.pull(rectangle(0.0, 0.0, 50.0, 30.0), at(0.0), 12.0);
    part.blend(FeatureKind::Chamfer, 2.0,
               {{25.0, 0.0, 12.0}, {50.0, 15.0, 12.0}, {25.0, 30.0, 12.0}, {0.0, 15.0, 12.0}});
    const double wanted = lcad::kernel::volume(part.shape());
    Document model;
    const MeshResult made = addMesh(model, part.mesh(), "Bevel");
    show("chamfer", made);
    REQUIRE(countOf(model, FeatureKind::Chamfer) == 1);
    CHECK_THAT(firstOf(model, FeatureKind::Chamfer).size, WithinAbs(2.0, 1e-3));
    CHECK_THAT(volumeOf(model, made), WithinRel(wanted, 1e-3));
}

TEST_CASE("A round along the top comes back as a Fillet", "[recover]") {
    Maker part;
    part.pull(rectangle(0.0, 0.0, 50.0, 30.0), at(0.0), 12.0);
    part.blend(FeatureKind::Fillet, 3.0,
               {{25.0, 0.0, 12.0}, {50.0, 15.0, 12.0}, {25.0, 30.0, 12.0}, {0.0, 15.0, 12.0}});
    const double wanted = lcad::kernel::volume(part.shape());
    Document model;
    const MeshResult made = addMesh(model, part.mesh(), "Round");
    show("top round", made);
    REQUIRE(countOf(model, FeatureKind::Fillet) >= 1);
    CHECK_THAT(firstOf(model, FeatureKind::Fillet).size, WithinAbs(3.0, 1e-3));
    CHECK_THAT(volumeOf(model, made), WithinRel(wanted, 1e-3));
}

TEST_CASE("A boss on a plate with a blind hole comes back in layers", "[recover]") {
    Maker part;
    part.pull(rectangle(0.0, 0.0, 60.0, 40.0), at(0.0), 5.0);
    part.pull(rectangle(15.0, 10.0, 45.0, 30.0), at(5.0), 10.0, Operation::Join);
    part.hole(at(15.0), {{30.0, 20.0}}, 8.0, 7.0);
    const double wanted = lcad::kernel::volume(part.shape());
    Document model;
    const MeshResult made = addMesh(model, part.mesh(), "Boss");
    show("boss", made);
    CHECK(countOf(model, FeatureKind::Pull) == 2);
    REQUIRE(countOf(model, FeatureKind::Hole) == 1);
    CHECK_THAT(firstOf(model, FeatureKind::Hole).size, WithinAbs(8.0, 1e-3));
    CHECK_THAT(firstOf(model, FeatureKind::Hole).distance, WithinAbs(7.0, 1e-3));
    CHECK_THAT(volumeOf(model, made), WithinRel(wanted, 1e-3));
}

// A block cut off slanted two ways, so that no one direction runs along all its faces.
void makeTent(Maker& part) {
    part.pull(rectangle(0.0, 0.0, 80.0, 60.0), at(0.0), 30.0);
    const auto closed = [](const std::vector<lcad::sketch::Point2>& corners) {
        lcad::sketch::Sketch s;
        std::vector<lcad::sketch::EntityId> lines;
        for (std::size_t i = 0; i < corners.size(); ++i) {
            lines.push_back(s.addLine(corners[i], corners[(i + 1) % corners.size()]));
        }
        for (std::size_t i = 0; i < lines.size(); ++i) {
            s.addConstraint(ConstraintKind::Coincident, s.find(lines[i])->end,
                            s.find(lines[(i + 1) % lines.size()])->start);
        }
        return s;
    };
    const Plane front = lcad::kernel::makePlane({0.0, -1.0, 0.0}, {0.0, -1.0, 0.0}, {1.0, 0.0, 0.0});
    part.pull(closed({{-1.0, 18.0}, {81.0, 31.0}, {-1.0, 31.0}}), front, -62.0, Operation::Cut);
    const Plane side = lcad::kernel::makePlane({-1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0});
    part.pull(closed({{-1.0, 19.0}, {61.0, 28.0}, {61.0, 32.0}, {-1.0, 32.0}}), side, 82.0,
              Operation::Cut);
}

const Plane under = lcad::kernel::makePlane({0.0, 0.0, 0.0}, {0.0, 0.0, -1.0}, {1.0, 0.0, 0.0});

TEST_CASE("Holes come back as Holes on a shape that is no stack of sketches", "[recover]") {
    Maker part;
    makeTent(part);
    // One plane, one size, so that they share a feature.
    part.hole(under, {{20.0, -20.0}, {60.0, -45.0}}, 6.0, 9.0);
    const double wanted = lcad::kernel::volume(part.shape());
    Document model;
    const MeshResult made = addMesh(model, part.mesh(), "Tent");
    REQUIRE(made.parts.size() == 1);
    INFO("note: " << made.parts[0].note);
    CHECK(made.parts[0].result == Result::FeaturesOnSolid);
    CHECK(made.parts[0].holes == 2);
    REQUIRE(countOf(model, FeatureKind::Hole) == 1);
    CHECK_THAT(firstOf(model, FeatureKind::Hole).size, WithinAbs(6.0, 1e-3));
    CHECK_THAT(firstOf(model, FeatureKind::Hole).distance, WithinAbs(9.0, 1e-3));
    CHECK_THAT(volumeOf(model, made), WithinRel(wanted, 1e-3));
}

TEST_CASE("A round on a shape that is no stack of sketches comes back as a Fillet", "[recover]") {
    Maker part;
    makeTent(part);
    part.blend(FeatureKind::Fillet, 4.0, {{40.0, 0.0, 0.0}});
    const double wanted = lcad::kernel::volume(part.shape());
    Document model;
    const MeshResult made = addMesh(model, part.mesh(), "Tent");
    REQUIRE(made.parts.size() == 1);
    INFO("note: " << made.parts[0].note);
    CHECK(made.parts[0].result == Result::FeaturesOnSolid);
    REQUIRE(countOf(model, FeatureKind::Fillet) >= 1);
    CHECK_THAT(firstOf(model, FeatureKind::Fillet).size, WithinAbs(4.0, 1e-3));
    CHECK_THAT(volumeOf(model, made), WithinRel(wanted, 1e-3));
}

TEST_CASE("A slot keeps its round ends in the sketch", "[recover]") {
    lcad::sketch::Sketch outline;
    const lcad::sketch::EntityId a = outline.addLine({0.0, 0.0}, {30.0, 0.0});
    const lcad::sketch::EntityId b = outline.addArc({30.0, 5.0}, {30.0, 0.0}, {30.0, 10.0});
    const lcad::sketch::EntityId c = outline.addLine({30.0, 10.0}, {0.0, 10.0});
    const lcad::sketch::EntityId d = outline.addArc({0.0, 5.0}, {0.0, 10.0}, {0.0, 0.0});
    outline.addConstraint(lcad::sketch::ConstraintKind::Coincident, outline.find(a)->end,
                          outline.find(b)->start);
    outline.addConstraint(lcad::sketch::ConstraintKind::Coincident, outline.find(b)->end,
                          outline.find(c)->start);
    outline.addConstraint(lcad::sketch::ConstraintKind::Coincident, outline.find(c)->end,
                          outline.find(d)->start);
    outline.addConstraint(lcad::sketch::ConstraintKind::Coincident, outline.find(d)->end,
                          outline.find(a)->start);
    Maker part;
    part.pull(outline, at(0.0), 6.0);
    const double wanted = lcad::kernel::volume(part.shape());
    Document model;
    const MeshResult made = addMesh(model, part.mesh(), "Slot");
    show("slot", made);
    CHECK(countOf(model, FeatureKind::Pull) == 1);
    CHECK(countOf(model, FeatureKind::Fillet) == 0);
    CHECK_THAT(volumeOf(model, made), WithinRel(wanted, 1e-3));
}

TEST_CASE("Round rims come back as Fillets", "[recover]") {
    lcad::sketch::Sketch disc;
    disc.addCircle({0.0, 0.0}, 30.0);
    Maker part;
    part.pull(disc, at(0.0), 12.0);
    part.hole(at(12.0), {{0.0, 0.0}}, 16.0, 12.0);
    // Where a round face has its seam, which is not an edge though the place is on one.
    part.blend(FeatureKind::Fillet, 2.0, {{30.0, 0.0, 12.0}});
    part.blend(FeatureKind::Fillet, 1.0, {{8.0, 0.0, 12.0}});
    const double wanted = lcad::kernel::volume(part.shape());
    Document model;
    const MeshResult made = addMesh(model, part.mesh(0.01), "Ring");
    show("ring", made);
    CHECK(countOf(model, FeatureKind::Fillet) == 2);
    CHECK_THAT(volumeOf(model, made), WithinRel(wanted, 1e-3));
}

TEST_CASE("A part that lies at an angle comes back as well", "[recover]") {
    Maker part;
    part.pull(rectangle(0.0, 0.0, 40.0, 20.0), at(0.0), 6.0);
    part.hole(at(6.0), {{10.0, 10.0}, {30.0, 10.0}}, 5.0, 6.0);
    const double wanted = lcad::kernel::volume(part.shape());
    Mesh turned = part.mesh();
    const double length = std::sqrt(1.0 + 1.0 + 0.04);
    const lcad::kernel::Placement tilt = lcad::kernel::rotation(
        lcad::kernel::Axis{{0.0, 0.0, 0.0}, {1.0 / length, 1.0 / length, 0.2 / length}}, 0.7);
    for (Point& p : turned.vertices) {
        p = lcad::kernel::apply(tilt, p);
    }
    Document model;
    const MeshResult made = addMesh(model, turned, "Tilted");
    show("tilted", made);
    CHECK(countOf(model, FeatureKind::Pull) == 1);
    CHECK(made.parts[0].holes == 2);
    CHECK_THAT(volumeOf(model, made), WithinRel(wanted, 1e-3));
}

TEST_CASE("A sphere stays a solid, having no features", "[recover]") {
    const lcad::kernel::Shape ball = lcad::kernel::makeSphere(20.0);
    Document model;
    const MeshResult made = addMesh(model, lcad::kernel::triangulate(ball, {0.01, 8.0}), "Ball");
    REQUIRE(made.parts.size() == 1);
    CHECK(made.parts[0].result != Result::Features);
    CHECK(made.parts[0].result != Result::Failed);
    CHECK(model.features().empty());
    REQUIRE(made.bodies.size() == 1);
    CHECK_THAT(volumeOf(model, made), WithinRel(4.0 / 3.0 * pi * 8000.0, 5e-3));
}

TEST_CASE("Several parts become several bodies, all one undo step", "[recover]") {
    Maker one;
    one.pull(rectangle(0.0, 0.0, 10.0, 10.0), at(0.0), 10.0);
    Maker two;
    two.pull(rectangle(20.0, 0.0, 35.0, 10.0), at(0.0), 5.0);
    Mesh both = one.mesh();
    const Mesh other = two.mesh();
    const auto offset = static_cast<std::uint32_t>(both.vertices.size());
    both.vertices.insert(both.vertices.end(), other.vertices.begin(), other.vertices.end());
    for (const auto& c : other.triangles) {
        both.triangles.push_back({c[0] + offset, c[1] + offset, c[2] + offset});
    }
    Document model;
    const MeshResult made = addMesh(model, both, "Pair");
    REQUIRE(made.parts.size() == 2);
    CHECK(made.bodies.size() == 2);
    CHECK(model.bodies().size() == 2);
    CHECK(model.undoLabel() == "Import Pair");
    model.undo();
    CHECK(model.bodies().empty());
    CHECK(model.features().empty());
    CHECK(model.sketches().empty());
}

TEST_CASE("A part with a hollow inside stays a solid and keeps the hollow", "[recover]") {
    Maker outer;
    outer.pull(rectangle(0.0, 0.0, 30.0, 30.0), at(0.0), 30.0);
    Maker inner;
    inner.pull(rectangle(10.0, 10.0, 20.0, 20.0), at(10.0), 10.0);
    Mesh both = outer.mesh();
    const Mesh hollow = inner.mesh();
    const auto offset = static_cast<std::uint32_t>(both.vertices.size());
    both.vertices.insert(both.vertices.end(), hollow.vertices.begin(), hollow.vertices.end());
    for (const auto& c : hollow.triangles) {
        // Facing inward.
        both.triangles.push_back({c[0] + offset, c[2] + offset, c[1] + offset});
    }
    Document model;
    const MeshResult made = addMesh(model, both, "Box");
    REQUIRE(made.parts.size() == 1);
    CHECK(made.parts[0].result == Result::ExactSolid);
    CHECK_THAT(volumeOf(model, made), WithinRel(27000.0 - 1000.0, 1e-6));
}

TEST_CASE("A mesh that does not close comes to nothing and says so", "[recover]") {
    Maker part;
    part.pull(rectangle(0.0, 0.0, 10.0, 10.0), at(0.0), 10.0);
    Mesh open = part.mesh();
    open.triangles.pop_back();
    Document model;
    const MeshResult made = addMesh(model, open, "Open");
    REQUIRE(made.parts.size() == 1);
    CHECK(made.parts[0].result == Result::Failed);
    CHECK(made.parts[0].note == "it is not closed");
    CHECK(made.bodies.empty());
    CHECK(model.bodies().empty());
}
