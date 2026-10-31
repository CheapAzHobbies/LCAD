// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/document/Document.h>
#include <lcad/document/File.h>
#include <lcad/kernel/Primitives.h>

#include <TempDir.h>

#include <catch2/catch_test_macros.hpp>

using namespace lcad::document;

namespace {

Drawing someDrawing(Document& document) {
    Drawing drawing;
    drawing.name = document.uniqueDrawingName("Drawing");
    drawing.size = SheetSize::Tabloid;
    drawing.standard = DrawingStandard::Ansi;
    drawing.titleBlock.title = "Bracket";
    drawing.titleBlock.partNumber = "LC-001";
    drawing.titleBlock.revision = "B";
    drawing.titleBlock.material = "6061 aluminum";

    DrawingView front;
    front.id = document.newId();
    front.kind = ViewKind::Front;
    front.scale = 0.5;
    front.x = 120.0;
    front.y = 90.0;
    drawing.views.push_back(front);

    DrawingView section;
    section.id = document.newId();
    section.kind = ViewKind::Section;
    section.label = "A";
    section.parent = front.id;
    section.sectionVertical = false;
    section.sectionPosition = 12.5;
    section.sectionFlipped = true;
    section.scale = 0.5;
    section.x = 260.0;
    section.y = 90.0;
    section.showHidden = false;
    drawing.views.push_back(section);

    DrawingView detail;
    detail.id = document.newId();
    detail.kind = ViewKind::Detail;
    detail.label = "B";
    detail.parent = front.id;
    detail.detailX = 3.0;
    detail.detailY = 4.0;
    detail.detailRadius = 8.0;
    detail.scale = 2.0;
    detail.x = 300.0;
    detail.y = 200.0;
    drawing.views.push_back(detail);

    DrawingDimension width;
    width.id = document.newId();
    width.view = front.id;
    width.kind = DimensionKind::Horizontal;
    width.first = {ObjectId{7}, {ObjectId{3}, ObjectId{4}}, "a|b", DrawingPoint::Where::Start};
    width.second = {ObjectId{7}, {}, "c|d", DrawingPoint::Where::End};
    width.offset = -12.0;
    width.suffix = "± 0.1";
    drawing.dimensions.push_back(width);

    DrawingDimension hole;
    hole.id = document.newId();
    hole.view = front.id;
    hole.kind = DimensionKind::Diameter;
    hole.first = {ObjectId{7}, {}, "e|f", DrawingPoint::Where::Center};
    drawing.dimensions.push_back(hole);

    DrawingNote note;
    note.id = document.newId();
    note.text = "Break all edges.\nDeburr.";
    note.x = 20.0;
    note.y = 30.0;
    note.height = 5.0;
    drawing.notes.push_back(note);
    return drawing;
}

Setup someSetup(Document& document, ObjectId tool) {
    Setup setup;
    setup.name = document.uniqueSetupName("Setup");
    setup.machine.name = "Shapeoko";
    setup.machine.dialect = PostDialect::LinuxCnc;
    setup.machine.travelX = 425.0;
    setup.machine.rapidRate = 5000.0;
    setup.stock.side = 4.0;
    setup.stock.top = 2.0;
    setup.stock.bottom = 1.0;
    setup.origin = WorkOrigin::StockTopCenter;

    MachiningOperation pocket;
    pocket.id = document.newId();
    pocket.name = "Pocket 1";
    pocket.kind = OperationKind::Pocket;
    pocket.tool = tool;
    pocket.targets.push_back({ObjectId{5}, {ObjectId{2}}, "x/top"});
    pocket.feed = 800.0;
    pocket.stepdown = 1.5;
    pocket.side = ContourSide::Inside;
    pocket.peck = 2.5;
    pocket.rampAngle = 7.5;
    pocket.tabs = 3;
    pocket.tabWidth = 4.5;
    pocket.tabHeight = 0.75;
    pocket.suppressed = true;
    setup.operations.push_back(pocket);
    return setup;
}

} // namespace

TEST_CASE("Drawings are added, changed, and removed as one undo step each", "[document][drawing]") {
    Document document;
    Drawing drawing = someDrawing(document);
    const ObjectId id = document.addDrawing(drawing);
    REQUIRE(document.drawings().size() == 1);
    CHECK(document.canUndo());
    CHECK(document.uniqueDrawingName("Drawing") == "Drawing 2");
    drawing.id = id;
    CHECK(*document.findDrawing(id) == drawing);

    Drawing changed = drawing;
    changed.views[0].scale = 2.0;
    document.changeDrawing(changed, "Change Scale");
    CHECK(document.undoLabel() == "Change Scale");
    CHECK(document.findDrawing(id)->views[0].scale == 2.0);
    document.undo();
    CHECK(document.findDrawing(id)->views[0].scale == 0.5);
    document.redo();
    CHECK(document.findDrawing(id)->views[0].scale == 2.0);

    document.removeDrawing(id);
    CHECK(document.drawings().empty());
    document.undo();
    CHECK(*document.findDrawing(id) == changed);
    document.undo();
    document.undo();
    CHECK(document.drawings().empty());

    Drawing unnamed;
    CHECK_THROWS_AS(document.addDrawing(unnamed), std::invalid_argument);
    drawing.views[0].scale = 0.0;
    CHECK_THROWS_AS(document.addDrawing(drawing), std::invalid_argument);
}

TEST_CASE("Tools and setups are kept with the document", "[document][machining]") {
    Document document;
    Tool mill;
    mill.name = "6 mm flat";
    const ObjectId tool = document.addTool(mill);
    const ObjectId setup = document.addSetup(someSetup(document, tool));
    REQUIRE(document.findSetup(setup) != nullptr);
    CHECK(document.findSetup(setup)->name == "Setup 1");

    // A tool an operation uses stays.
    CHECK_THROWS_AS(document.removeTool(tool), std::invalid_argument);
    Setup without = *document.findSetup(setup);
    without.operations.clear();
    document.changeSetup(without);
    document.removeTool(tool);
    CHECK(document.tools().empty());
    document.undo();
    CHECK(document.findTool(tool) != nullptr);
    document.undo();
    CHECK(document.findSetup(setup)->operations.size() == 1);

    Tool bad;
    bad.name = "No size";
    bad.diameter = 0.0;
    CHECK_THROWS_AS(document.addTool(bad), std::invalid_argument);
}

TEST_CASE("Drawings, tools, and setups come back from a file as they were",
          "[document][drawing][machining][file]") {
    const lcad::test::TempDir folder;
    const auto file = folder.path() / "part.lcad";
    Document document;
    document.addBody("Plate", lcad::kernel::makeBox(10.0, 10.0, 10.0));
    const ObjectId drawing = document.addDrawing(someDrawing(document));
    Tool mill;
    mill.name = "6 mm flat";
    mill.kind = ToolKind::BallEndMill;
    mill.diameter = 6.35;
    mill.flutes = 3;
    mill.number = 4;
    const ObjectId tool = document.addTool(mill);
    const ObjectId setup = document.addSetup(someSetup(document, tool));
    saveDocument(document, file);

    const LoadedDocument loaded = loadDocument(file);
    REQUIRE(loaded.document->drawings().size() == 1);
    CHECK(*loaded.document->findDrawing(drawing) == *document.findDrawing(drawing));
    CHECK(*loaded.document->findTool(tool) == *document.findTool(tool));
    CHECK(*loaded.document->findSetup(setup) == *document.findSetup(setup));
    // What is added later does not take an id already in use.
    Document& reopened = *loaded.document;
    CHECK(reopened.newId().value > setup.value);
}
