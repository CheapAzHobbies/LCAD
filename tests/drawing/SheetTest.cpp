// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/document/Document.h>
#include <lcad/drawing/Dxf.h>
#include <lcad/drawing/Sheet.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/Transform.h>
#include <lcad/sketch/Edit.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

using namespace lcad::document;
using namespace lcad::drawing;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
namespace kernel = lcad::kernel;

namespace {

constexpr double pi = 3.14159265358979323846;

// A plate of 60 by 40 by 10 with a hole of radius 5 through it at (30, 20).
kernel::NamedShape plate() {
    const kernel::NamedShape box = kernel::nameFaces(kernel::makeBox(60.0, 40.0, 10.0), "p");
    const kernel::NamedShape hole = kernel::nameFaces(
        kernel::translated(kernel::makeCylinder(5.0, 12.0), 30.0, 20.0, -1.0), "h");
    return kernel::combine(box, hole, kernel::Combination::Cut);
}

ObjectId addPlate(Document& document) {
    return document.addBody("Plate", plate().shape);
}

Drawing frontView(Document& document, ViewKind kind = ViewKind::Front) {
    Drawing drawing;
    drawing.name = "Drawing 1";
    DrawingView view;
    view.id = document.newId();
    view.kind = kind;
    view.scale = 1.0;
    view.x = 150.0;
    view.y = 150.0;
    drawing.views.push_back(view);
    return drawing;
}

int countStrokes(const Sheet& sheet, Pen pen) {
    return static_cast<int>(std::count_if(sheet.strokes.begin(), sheet.strokes.end(),
                                          [pen](const Stroke& s) { return s.pen == pen; }));
}

double lengthOf(const Stroke& stroke) {
    double total = 0.0;
    for (std::size_t i = 1; i < stroke.points.size(); ++i) {
        total += std::hypot(stroke.points[i].x - stroke.points[i - 1].x,
                            stroke.points[i].y - stroke.points[i - 1].y);
    }
    return total;
}

double lengthWith(const Sheet& sheet, Pen pen) {
    double total = 0.0;
    for (const Stroke& stroke : sheet.strokes) {
        total += stroke.pen == pen ? lengthOf(stroke) : 0.0;
    }
    return total;
}

bool hasLabel(const Sheet& sheet, const std::string& text) {
    return std::any_of(sheet.labels.begin(), sheet.labels.end(),
                       [&text](const Label& label) { return label.text == text; });
}

// The extreme points of the snap points of a view, by what is picked in it.
const SnapPoint& snapAt(const Sheet& sheet, ObjectId view, double x, double y) {
    const SnapPoint* best = nullptr;
    double nearest = 1e300;
    for (const SnapPoint& point : sheet.snapPoints) {
        if (point.view != view) {
            continue;
        }
        const double away = std::hypot(point.at.x - x, point.at.y - y);
        if (away < nearest) {
            nearest = away;
            best = &point;
        }
    }
    REQUIRE(best != nullptr);
    return *best;
}

} // namespace

TEST_CASE("Sheets have their standard sizes and a scale that reads right", "[drawing]") {
    CHECK(sheetDimensions(SheetSize::A3).width == 420.0);
    CHECK(sheetDimensions(SheetSize::A4).height == 210.0);
    CHECK_THAT(sheetDimensions(SheetSize::Letter).width, WithinAbs(279.4, 1e-9));
    CHECK(scaleText(0.5) == "1:2");
    CHECK(scaleText(2.0) == "2:1");
    CHECK(scaleText(1.0) == "1:1");
    CHECK(lengthText(10.0) == "10");
    CHECK(lengthText(12.5) == "12.5");
    CHECK(lengthText(3.14159) == "3.14");
    CHECK(fittingScale(100.0, 50.0, 300.0, 200.0) == 2.0);
    CHECK(fittingScale(1000.0, 50.0, 300.0, 200.0) == 0.2);
}

TEST_CASE("A view is the model flattened at its scale, about its place on the sheet", "[drawing]") {
    Document document;
    addPlate(document);
    Drawing drawing = frontView(document);
    drawing.views[0].scale = 2.0;
    SheetBuilder builder(document);
    const Sheet sheet = builder.build(drawing);
    CHECK(sheet.problems.empty());
    CHECK(sheet.width == 420.0);

    REQUIRE(sheet.views.size() == 1);
    const ViewBox& box = sheet.views[0];
    CHECK_THAT(box.right - box.left, WithinAbs(120.0, 1e-6));
    CHECK_THAT(box.top - box.bottom, WithinAbs(20.0, 1e-6));
    CHECK_THAT((box.left + box.right) / 2.0, WithinAbs(150.0, 1e-6));
    CHECK_THAT((box.bottom + box.top) / 2.0, WithinAbs(150.0, 1e-6));

    // The front's outline, and the hole's sides behind it, dashed.
    CHECK_THAT(lengthWith(sheet, Pen::Visible), WithinAbs(2.0 * (120.0 + 20.0), 1e-6));
    CHECK_THAT(lengthWith(sheet, Pen::Hidden), WithinAbs(2.0 * 20.0, 1e-6));

    drawing.views[0].showHidden = false;
    CHECK(countStrokes(builder.build(drawing), Pen::Hidden) == 0);
}

TEST_CASE("A view follows the model", "[drawing]") {
    Document document;
    const ObjectId body = addPlate(document);
    const Drawing drawing = frontView(document);
    SheetBuilder builder(document);
    const double before = builder.build(drawing).views[0].right;

    // The builder keeps what it worked out until it is told the model changed.
    document.removeBody(body);
    document.addBody("Larger", kernel::makeBox(100.0, 40.0, 10.0));
    builder.invalidate();
    const Sheet after = builder.build(drawing);
    CHECK_THAT(after.views[0].right - after.views[0].left, WithinAbs(100.0, 1e-6));
    CHECK(after.views[0].right != before);
}

TEST_CASE("Circles get center marks, and arcs keep their form to the end", "[drawing]") {
    Document document;
    addPlate(document);
    Drawing drawing = frontView(document, ViewKind::Top);
    SheetBuilder builder(document);
    const Sheet sheet = builder.build(drawing);
    CHECK(countStrokes(sheet, Pen::Center) == 2);
    const auto circle =
        std::find_if(sheet.strokes.begin(), sheet.strokes.end(),
                     [](const Stroke& s) { return s.pen == Pen::Visible && s.isArc; });
    REQUIRE(circle != sheet.strokes.end());
    CHECK_THAT(circle->radius, WithinAbs(5.0, 1e-6));
    CHECK_THAT(circle->center.x, WithinAbs(150.0, 1e-6));
    CHECK_THAT(circle->center.y, WithinAbs(150.0, 1e-6));

    drawing.views[0].centerMarks = false;
    CHECK(countStrokes(builder.build(drawing), Pen::Center) == 0);
}

TEST_CASE("A section shows the cut faces hatched, and where it was cut on its parent",
          "[drawing]") {
    Document document;
    addPlate(document);
    Drawing drawing = frontView(document, ViewKind::Top);
    DrawingView section;
    section.id = document.newId();
    section.kind = ViewKind::Section;
    section.label = "A";
    section.parent = drawing.views[0].id;
    section.sectionVertical = false;
    // Across the middle of the hole: Y = 20 in the view from above.
    section.sectionPosition = 20.0;
    section.scale = 1.0;
    section.x = 300.0;
    section.y = 150.0;
    section.showHidden = false;
    drawing.views.push_back(section);

    SheetBuilder builder(document);
    const Sheet sheet = builder.build(drawing);
    CHECK(sheet.problems.empty());
    CHECK(countStrokes(sheet, Pen::Hatch) > 10);
    CHECK(hasLabel(sheet, "SECTION A-A"));
    // The letters at the ends of the cutting line.
    CHECK(std::count_if(sheet.labels.begin(), sheet.labels.end(),
                        [](const Label& label) { return label.text == "A"; }) == 2);

    // The hatch lies on the two cut faces, either side of the hole, and not across it.
    const ViewBox& box = sheet.views[1];
    const double middle = (box.left + box.right) / 2.0;
    CHECK_THAT(box.right - box.left, WithinAbs(60.0, 1e-6));
    CHECK_THAT(box.top - box.bottom, WithinAbs(10.0, 1e-6));
    for (const Stroke& stroke : sheet.strokes) {
        if (stroke.pen != Pen::Hatch) {
            continue;
        }
        for (const Point2D& p : stroke.points) {
            CHECK(std::abs(p.x - middle) >= 5.0 - 1e-6);
            CHECK(p.y >= box.bottom - 1e-6);
            CHECK(p.y <= box.top + 1e-6);
        }
    }

    // A cut past the model says so, and the rest of the sheet still stands.
    drawing.views[1].sectionPosition = 200.0;
    builder.invalidate();
    const Sheet missed = builder.build(drawing);
    REQUIRE(missed.problems.size() == 1);
    CHECK_THAT(missed.problems[0], ContainsSubstring("Section A"));
    CHECK(missed.views.size() == 1);
}

TEST_CASE("A detail shows a round part of its parent, larger", "[drawing]") {
    Document document;
    addPlate(document);
    Drawing drawing = frontView(document, ViewKind::Top);
    DrawingView detail;
    detail.id = document.newId();
    detail.kind = ViewKind::Detail;
    detail.label = "B";
    detail.parent = drawing.views[0].id;
    // About the corner of the plate at (60, 40), so that a quarter of the circle shows the plate.
    detail.detailX = 60.0;
    detail.detailY = 40.0;
    detail.detailRadius = 10.0;
    detail.scale = 3.0;
    detail.x = 300.0;
    detail.y = 150.0;
    drawing.views.push_back(detail);

    SheetBuilder builder(document);
    const Sheet sheet = builder.build(drawing);
    CHECK(sheet.problems.empty());
    CHECK(hasLabel(sheet, "DETAIL B (3:1)"));
    CHECK(hasLabel(sheet, "B"));
    // Two edges of the plate run from the corner out to the circle, three times as long, and
    // nothing else of the parent's lines is drawn around the detail.
    double inside = 0.0;
    for (const Stroke& stroke : sheet.strokes) {
        if (stroke.pen != Pen::Visible) {
            continue;
        }
        for (std::size_t i = 1; i < stroke.points.size(); ++i) {
            const Point2D& a = stroke.points[i - 1];
            const Point2D& b = stroke.points[i];
            if (std::hypot(a.x - 300.0, a.y - 150.0) <= 30.0 + 1e-6 &&
                std::hypot(b.x - 300.0, b.y - 150.0) <= 30.0 + 1e-6) {
                inside += std::hypot(b.x - a.x, b.y - a.y);
            }
        }
    }
    CHECK_THAT(inside, WithinAbs(2.0 * 30.0, 1e-6));
    REQUIRE(sheet.views.size() == 2);
    CHECK_THAT(sheet.views[1].right - sheet.views[1].left, WithinAbs(60.0, 1e-6));
}

TEST_CASE("Dimensions measure the model, follow it, and say when they lose it", "[drawing]") {
    Document document;
    const ObjectId body = addPlate(document);
    Drawing drawing = frontView(document, ViewKind::Top);
    const ObjectId view = drawing.views[0].id;
    drawing.views[0].scale = 2.0;
    SheetBuilder builder(document);
    const Sheet first = builder.build(drawing);
    // The top view's corners, at twice the size about the middle of the sheet's view.
    const SnapPoint& left = snapAt(first, view, 150.0 - 60.0, 150.0 - 40.0);
    const SnapPoint& right = snapAt(first, view, 150.0 + 60.0, 150.0 - 40.0);
    const SnapPoint& upper = snapAt(first, view, 150.0 + 60.0, 150.0 + 40.0);
    REQUIRE(first.snapCircles.size() >= 1);
    CHECK_THAT(first.snapCircles[0].radius, WithinAbs(10.0, 1e-6));

    DrawingDimension width;
    width.id = document.newId();
    width.view = view;
    width.kind = DimensionKind::Horizontal;
    width.first = left.point;
    width.second = right.point;
    width.offset = -12.0;
    drawing.dimensions.push_back(width);

    DrawingDimension height;
    height.id = document.newId();
    height.view = view;
    height.kind = DimensionKind::Vertical;
    height.first = right.point;
    height.second = upper.point;
    height.offset = -14.0;
    height.suffix = " ±0.1";
    drawing.dimensions.push_back(height);

    DrawingDimension across;
    across.id = document.newId();
    across.view = view;
    across.kind = DimensionKind::Aligned;
    across.first = left.point;
    across.second = upper.point;
    across.offset = 6.0;
    drawing.dimensions.push_back(across);

    DrawingDimension hole;
    hole.id = document.newId();
    hole.view = view;
    hole.kind = DimensionKind::Diameter;
    hole.first = first.snapCircles[0].point;
    drawing.dimensions.push_back(hole);

    DrawingDimension round = hole;
    round.id = document.newId();
    round.kind = DimensionKind::Radius;
    drawing.dimensions.push_back(round);

    const Sheet sheet = builder.build(drawing);
    CHECK(sheet.lostDimensions.empty());
    REQUIRE(sheet.dimensions.size() == 5);
    CHECK_THAT(sheet.dimensions[0].value, WithinAbs(60.0, 1e-9));
    CHECK(sheet.dimensions[0].text == "60");
    CHECK_THAT(sheet.dimensions[1].value, WithinAbs(40.0, 1e-9));
    CHECK(sheet.dimensions[1].text == "40 ±0.1");
    CHECK_THAT(sheet.dimensions[2].value, WithinAbs(std::hypot(60.0, 40.0), 1e-9));
    CHECK(sheet.dimensions[2].text == "72.11");
    CHECK(sheet.dimensions[3].text == "Ø10");
    CHECK(sheet.dimensions[4].text == "R5");
    // Each has its arrows.
    CHECK(sheet.filled.size() == 2 * 3 + 2);
    CHECK(countStrokes(sheet, Pen::Dimension) == 3 * 3 + 2 * 2);

    // The plate is replaced by a thing without those edges: the dimensions stay, and say so.
    document.removeBody(body);
    document.addBody("Other", kernel::makeBox(10.0, 10.0, 10.0));
    builder.invalidate();
    const Sheet lost = builder.build(drawing);
    CHECK(lost.lostDimensions.size() == 5);
    CHECK(lost.dimensions.empty());
    REQUIRE_FALSE(lost.problems.empty());
    CHECK_THAT(lost.problems.back(), ContainsSubstring("5 dimensions have lost"));
    CHECK(drawing.dimensions.size() == 5);
}

TEST_CASE("Dimensions go by names, so a changed model keeps them", "[drawing]") {
    Document document;
    const ObjectId sketch = document.addSketch("Outline", kernel::planeXY());
    lcad::sketch::Sketch outline;
    const lcad::sketch::RectangleParts sides =
        lcad::sketch::addRectangle(outline, {0.0, 0.0}, {60.0, 40.0});
    outline.addConstraint(lcad::sketch::ConstraintKind::Coincident,
                          outline.find(sides.bottom)->start, outline.origin());
    const lcad::sketch::ConstraintId wide = outline.addConstraint(
        lcad::sketch::ConstraintKind::Distance, outline.find(sides.bottom)->start,
        outline.find(sides.bottom)->end, 0, 60.0);
    outline.addConstraint(lcad::sketch::ConstraintKind::Distance, outline.find(sides.right)->start,
                          outline.find(sides.right)->end, 0, 40.0);
    document.setSketchGeometry(sketch, outline, "Rectangle");
    Feature pull;
    pull.kind = FeatureKind::Pull;
    pull.sketch = sketch;
    pull.distance = 10.0;
    pull.operation = Operation::NewBody;
    document.addFeature(pull);

    Drawing drawing = frontView(document, ViewKind::Top);
    const ObjectId view = drawing.views[0].id;
    SheetBuilder builder(document);
    const Sheet before = builder.build(drawing);
    const SnapPoint& a = snapAt(before, view, 150.0 - 30.0, 150.0 - 20.0);
    const SnapPoint& b = snapAt(before, view, 150.0 + 30.0, 150.0 - 20.0);

    DrawingDimension width;
    width.id = document.newId();
    width.view = view;
    width.kind = DimensionKind::Horizontal;
    width.first = a.point;
    width.second = b.point;
    drawing.dimensions.push_back(width);
    REQUIRE(builder.build(drawing).dimensions.size() == 1);
    CHECK_THAT(builder.build(drawing).dimensions[0].value, WithinAbs(60.0, 1e-9));

    // A wider plate is the same plate: the edges have their names, and the dimension its value.
    lcad::sketch::Sketch wider = document.findSketch(sketch)->geometry;
    REQUIRE(lcad::sketch::tryDimensionValue(wider, wide, 90.0).ok());
    document.setSketchGeometry(sketch, wider, "Change Dimension");
    builder.invalidate();
    const Sheet after = builder.build(drawing);
    CHECK(after.lostDimensions.empty());
    REQUIRE(after.dimensions.size() == 1);
    CHECK_THAT(after.dimensions[0].value, WithinAbs(90.0, 1e-9));
}

TEST_CASE("Hatch lines and center marks stay inside a view of a section's section", "[drawing]") {
    Document document;
    addPlate(document);
    Drawing drawing = frontView(document, ViewKind::Top);
    DrawingView section;
    section.id = document.newId();
    section.kind = ViewKind::Section;
    section.label = "A";
    section.parent = drawing.views[0].id;
    section.sectionVertical = true;
    section.sectionPosition = 30.0;
    section.scale = 1.0;
    section.x = 300.0;
    section.y = 150.0;
    drawing.views.push_back(section);

    DrawingView detail;
    detail.id = document.newId();
    detail.kind = ViewKind::Detail;
    detail.label = "C";
    detail.parent = section.id;
    detail.detailRadius = 6.0;
    detail.scale = 4.0;
    detail.x = 100.0;
    detail.y = 250.0;
    drawing.views.push_back(detail);

    SheetBuilder builder(document);
    const Sheet sheet = builder.build(drawing);
    CHECK(sheet.problems.empty());
    CHECK(hasLabel(sheet, "DETAIL C (4:1)"));
}

TEST_CASE("The title block says what the drawing is", "[drawing]") {
    Document document;
    addPlate(document);
    Drawing drawing = frontView(document);
    drawing.views[0].scale = 0.5;
    drawing.titleBlock.title = "Mounting plate";
    drawing.titleBlock.partNumber = "LC-0042";
    drawing.titleBlock.revision = "C";
    drawing.titleBlock.material = "6061-T6";
    SheetBuilder builder(document);
    const Sheet sheet = builder.build(drawing);
    for (const char* text : {"Mounting plate", "LC-0042", "C", "6061-T6", "1:2", "A3", "Title"}) {
        CHECK(hasLabel(sheet, text));
    }
    // Inside the border.
    for (const Label& label : sheet.labels) {
        CHECK(label.at.x > 10.0);
        CHECK(label.at.x < sheet.width - 10.0);
        CHECK(label.at.y > 10.0);
        CHECK(label.at.y < sheet.height - 10.0);
    }
}

TEST_CASE("Standard views fit the sheet, and go where the standard puts them", "[drawing]") {
    for (const DrawingStandard standard : {DrawingStandard::Iso, DrawingStandard::Ansi}) {
        Document document;
        addPlate(document);
        Drawing drawing;
        drawing.name = "Drawing 1";
        drawing.standard = standard;
        addStandardViews(document, drawing);
        REQUIRE(drawing.views.size() == 4);
        CHECK(drawing.views[0].kind == ViewKind::Front);
        CHECK(drawing.views[1].kind == ViewKind::Top);
        CHECK(drawing.views[2].kind == ViewKind::Right);
        CHECK(drawing.views[3].kind == ViewKind::Isometric);

        // First-angle puts the top view below the front, third-angle above it; the view from the
        // right goes on the other side likewise.
        if (standard == DrawingStandard::Iso) {
            CHECK(drawing.views[1].y < drawing.views[0].y);
            CHECK(drawing.views[2].x < drawing.views[0].x);
        } else {
            CHECK(drawing.views[1].y > drawing.views[0].y);
            CHECK(drawing.views[2].x > drawing.views[0].x);
        }
        CHECK_THAT(drawing.views[1].x, WithinAbs(drawing.views[0].x, 1e-9));
        CHECK_THAT(drawing.views[2].y, WithinAbs(drawing.views[0].y, 1e-9));

        SheetBuilder builder(document);
        const Sheet sheet = builder.build(drawing);
        CHECK(sheet.problems.empty());
        REQUIRE(sheet.views.size() == 4);
        const double titleTop = 10.0 + 32.0;
        for (std::size_t i = 0; i < sheet.views.size(); ++i) {
            const ViewBox& box = sheet.views[i];
            CHECK(box.left >= 10.0);
            CHECK(box.right <= sheet.width - 10.0);
            CHECK(box.bottom >= titleTop);
            CHECK(box.top <= sheet.height - 10.0);
            for (std::size_t j = i + 1; j < sheet.views.size(); ++j) {
                const ViewBox& other = sheet.views[j];
                const bool apart = box.right < other.left || other.right < box.left ||
                                   box.top < other.bottom || other.top < box.bottom;
                CHECK(apart);
            }
        }
    }

    // Nothing to show: no views.
    Document empty;
    Drawing drawing;
    drawing.name = "Drawing 1";
    addStandardViews(empty, drawing);
    CHECK(drawing.views.empty());
}

TEST_CASE("A sheet with no body in the document says so", "[drawing]") {
    Document document;
    const Drawing drawing = frontView(document);
    SheetBuilder builder(document);
    const Sheet sheet = builder.build(drawing);
    REQUIRE(sheet.problems.size() == 1);
    CHECK_THAT(sheet.problems[0], ContainsSubstring("no body"));
}

TEST_CASE("DXF keeps lines and circles, on layers of their own", "[drawing][dxf]") {
    Document document;
    addPlate(document);
    Drawing drawing = frontView(document, ViewKind::Top);
    SheetBuilder builder(document);
    const Sheet sheet = builder.build(drawing);
    const std::string text = dxfText(sheet);

    // Read back as DXF is: pairs of lines, a group code and a value.
    std::istringstream in(text);
    std::string code;
    std::string value;
    std::vector<std::pair<int, std::string>> groups;
    while (std::getline(in, code) && std::getline(in, value)) {
        groups.emplace_back(std::stoi(code), value);
    }
    REQUIRE(groups.back() == std::pair<int, std::string>(0, "EOF"));

    std::set<std::string> sections;
    std::set<std::string> layers;
    int circles = 0;
    int lines = 0;
    double circleRadius = 0.0;
    double circleX = 0.0;
    for (std::size_t i = 0; i + 1 < groups.size(); ++i) {
        if (groups[i] == std::pair<int, std::string>(0, "SECTION")) {
            sections.insert(groups[i + 1].second);
        }
        if (groups[i].first == 0 && groups[i].second == "LAYER") {
            layers.insert(groups[i + 1].second);
        }
        if (groups[i].first == 0 && groups[i].second == "LINE") {
            ++lines;
        }
        if (groups[i].first == 0 && groups[i].second == "CIRCLE" &&
            groups[i + 1].second == "VISIBLE") {
            ++circles;
            for (std::size_t j = i + 1; groups[j].first != 0; ++j) {
                if (groups[j].first == 40) {
                    circleRadius = std::stod(groups[j].second);
                } else if (groups[j].first == 10) {
                    circleX = std::stod(groups[j].second);
                }
            }
        }
    }
    CHECK(sections == std::set<std::string>{"HEADER", "TABLES", "ENTITIES"});
    CHECK(layers.count("VISIBLE") == 1);
    CHECK(layers.count("HIDDEN") == 1);
    CHECK(circles == 1);
    CHECK_THAT(circleRadius, WithinAbs(5.0, 1e-6));
    CHECK_THAT(circleX, WithinAbs(150.0, 1e-6));
    CHECK(lines >= 4);
    CHECK_THAT(text, ContainsSubstring("AC1009"));

    // An arc is an ARC, with its angles in degrees.
    Stroke arc;
    arc.pen = Pen::Visible;
    arc.isArc = true;
    arc.center = {1.0, 2.0};
    arc.radius = 3.0;
    arc.startAngle = 0.0;
    arc.endAngle = pi / 2.0;
    Sheet tiny;
    tiny.width = 10.0;
    tiny.height = 10.0;
    tiny.strokes.push_back(arc);
    const std::string arcText = dxfText(tiny);
    CHECK_THAT(arcText, ContainsSubstring("ARC\n8\nVISIBLE"));
    CHECK_THAT(arcText, ContainsSubstring("51\n90.000000"));
}
