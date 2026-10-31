// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/cam/Gcode.h>
#include <lcad/cam/Planner.h>
#include <lcad/cam/Simulation.h>
#include <lcad/document/Document.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/kernel/Transform.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <clocale>
#include <cmath>
#include <sstream>

using namespace lcad::cam;
using namespace lcad::document;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
namespace kernel = lcad::kernel;

namespace {

Tool flatMill(double diameter = 6.0) {
    Tool tool;
    tool.name = "Flat mill";
    tool.kind = ToolKind::FlatEndMill;
    tool.diameter = diameter;
    tool.fluteLength = 25.0;
    tool.stickout = 40.0;
    return tool;
}

MachiningOperation cut(OperationKind kind) {
    MachiningOperation operation;
    operation.name = "Operation";
    operation.kind = kind;
    operation.stepover = 2.4;
    operation.stepdown = 2.0;
    return operation;
}

Heights heights(double top, double bottom) {
    Heights h;
    h.top = top;
    h.bottom = bottom;
    h.retract = top + 2.0;
    h.clearance = top + 5.0;
    return h;
}

kernel::FlatRegion square(double size) {
    kernel::FlatRegion region;
    region.outer = {{0.0, 0.0}, {size, 0.0}, {size, size}, {0.0, size}};
    return region;
}

double maxOf(const std::vector<Move>& moves, double kernel::Point::*axis) {
    double most = -1e300;
    for (const Move& move : moves) {
        most = std::max(most, move.to.*axis);
    }
    return most;
}

double minOf(const std::vector<Move>& moves, double kernel::Point::*axis) {
    double least = 1e300;
    for (const Move& move : moves) {
        least = std::min(least, move.to.*axis);
    }
    return least;
}

// A pocket of 40 by 30, 6 deep, in a plate of 80 by 60 by 12, and a hole of 6 through it beside.
struct Part {
    Document document;
    ObjectId body;
    ObjectId mill;
    ObjectId drill;
    std::string floor;
    std::string bottom;
    std::string hole;
    kernel::Mesh mesh;

    Part() {
        const kernel::NamedShape plate =
            kernel::nameFaces(kernel::makeBox(80.0, 60.0, 12.0), "plate");
        const kernel::NamedShape pocket = kernel::nameFaces(
            kernel::translated(kernel::makeBox(40.0, 30.0, 10.0), 10.0, 15.0, 6.0), "pocket");
        const kernel::NamedShape bore = kernel::nameFaces(
            kernel::translated(kernel::makeCylinder(3.0, 14.0), 65.0, 30.0, -1.0), "bore");
        const kernel::Shape shape =
            kernel::combine(kernel::combine(plate, pocket, kernel::Combination::Cut), bore,
                            kernel::Combination::Cut)
                .shape;
        body = document.addBody("Plate", shape);
        const kernel::NamedShape named = document.namedBody(body);
        for (int i = 0; i < kernel::subShapeCount(named.shape, kernel::SubShapeKind::Face); ++i) {
            const kernel::Shape face = kernel::subShape(named.shape, kernel::SubShapeKind::Face, i);
            if (const auto plane = kernel::planeOfFace(face)) {
                if (plane->normal.z > 0.5 && std::abs(plane->origin.z - 6.0) < 1e-9) {
                    floor = named.faceNames[static_cast<std::size_t>(i)];
                }
                if (plane->normal.z < -0.5) {
                    bottom = named.faceNames[static_cast<std::size_t>(i)];
                }
            } else if (kernel::roundFaceAlongZ(face).has_value()) {
                hole = named.faceNames[static_cast<std::size_t>(i)];
            }
        }
        REQUIRE_FALSE(floor.empty());
        REQUIRE_FALSE(bottom.empty());
        REQUIRE_FALSE(hole.empty());
        Tool tool = flatMill(6.0);
        tool.number = 1;
        mill = document.addTool(tool);
        Tool twist;
        twist.name = "Drill";
        twist.kind = ToolKind::Drill;
        twist.diameter = 6.0;
        twist.fluteLength = 40.0;
        twist.stickout = 50.0;
        twist.number = 2;
        drill = document.addTool(twist);
        mesh = kernel::triangulate(shape, {0.01, 10.0});
    }

    [[nodiscard]] Setup setup() {
        Setup made;
        made.name = "Setup 1";
        made.stock.side = 5.0;
        made.stock.top = 1.0;
        made.stock.bottom = 1.0;

        MachiningOperation face = cut(OperationKind::Face);
        face.id = document.newId();
        face.name = "Face";
        face.tool = mill;
        face.stepdown = 1.0;
        made.operations.push_back(face);

        MachiningOperation pocket = cut(OperationKind::Pocket);
        pocket.id = document.newId();
        pocket.name = "Pocket";
        pocket.tool = mill;
        pocket.targets.push_back({body, {}, floor});
        made.operations.push_back(pocket);

        MachiningOperation contour = cut(OperationKind::Contour);
        contour.id = document.newId();
        contour.name = "Outside";
        contour.tool = mill;
        contour.side = ContourSide::Outside;
        contour.targets.push_back({body, {}, bottom});
        made.operations.push_back(contour);

        MachiningOperation holes = cut(OperationKind::Drill);
        holes.id = document.newId();
        holes.name = "Holes";
        holes.tool = drill;
        holes.feed = 200.0;
        holes.plungeFeed = 150.0;
        holes.peck = 4.0;
        holes.targets.push_back({body, {}, hole});
        made.operations.push_back(holes);
        return made;
    }
};

} // namespace

TEST_CASE("Passes step down to the bottom and stop there", "[cam]") {
    CHECK(passDepths(10.0, 4.0, 2.0) == std::vector<double>{8.0, 6.0, 4.0});
    CHECK(passDepths(10.0, 5.0, 2.0) == std::vector<double>{8.0, 6.0, 5.0});
    CHECK(passDepths(10.0, 9.0, 2.0) == std::vector<double>{9.0});
    CHECK(passDepths(10.0, 10.0, 2.0).empty());
    CHECK(passDepths(10.0, 4.0, 0.0).empty());
}

TEST_CASE("A pocket stays a tool's radius from its walls, and clears all it can reach", "[cam]") {
    const Tool tool = flatMill(6.0);
    std::vector<std::string> problems;
    const std::vector<Move> moves =
        pocketMoves(square(40.0), heights(10.0, 4.0), tool, cut(OperationKind::Pocket), problems);
    REQUIRE(problems.empty());
    REQUIRE_FALSE(moves.empty());

    // Everything where it cuts is within the walls by the tool's radius, and reaches it.
    double left = 1e300;
    double right = -1e300;
    for (const Move& move : moves) {
        if (move.kind == MoveKind::Feed) {
            left = std::min(left, move.to.x);
            right = std::max(right, move.to.x);
            CHECK(move.to.x >= 3.0 - 1e-3);
            CHECK(move.to.x <= 37.0 + 1e-3);
            CHECK(move.to.y >= 3.0 - 1e-3);
            CHECK(move.to.y <= 37.0 + 1e-3);
            CHECK(move.feed == 600.0);
        }
    }
    CHECK_THAT(left, WithinAbs(3.0, 1e-3));
    CHECK_THAT(right, WithinAbs(37.0, 1e-3));
    CHECK_THAT(minOf(moves, &kernel::Point::z), WithinAbs(4.0, 1e-9));
    CHECK(moves.back().to.z == 15.0);
    // Three depths, with the tool going down at the plunge feed and not sideways as it does.
    std::vector<double> levels;
    for (const Move& move : moves) {
        if (move.kind == MoveKind::Plunge) {
            levels.push_back(move.to.z);
            CHECK(move.feed == 200.0);
        }
    }
    std::sort(levels.begin(), levels.end());
    levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
    CHECK(levels == std::vector<double>{4.0, 6.0, 8.0});
}

TEST_CASE("Islands in a pocket are left standing", "[cam]") {
    kernel::FlatRegion region = square(60.0);
    region.holes.push_back({{20.0, 20.0}, {20.0, 40.0}, {40.0, 40.0}, {40.0, 20.0}});
    std::vector<std::string> problems;
    const std::vector<Move> moves =
        pocketMoves(region, heights(5.0, 0.0), flatMill(6.0), cut(OperationKind::Pocket), problems);
    REQUIRE(problems.empty());
    for (const Move& move : moves) {
        if (move.kind == MoveKind::Rapid) {
            continue;
        }
        // A tool's radius from the island, around its corners as well.
        const double dx = std::max({20.0 - move.to.x, 0.0, move.to.x - 40.0});
        const double dy = std::max({20.0 - move.to.y, 0.0, move.to.y - 40.0});
        CHECK(std::hypot(dx, dy) >= 3.0 - 1e-2);
    }
}

TEST_CASE("What cannot be done safely is refused, and says why", "[cam]") {
    std::vector<std::string> problems;
    MachiningOperation operation = cut(OperationKind::Pocket);

    SECTION("A tool too large for the pocket") {
        CHECK(pocketMoves(square(5.0), heights(5.0, 0.0), flatMill(6.0), operation, problems)
                  .empty());
        REQUIRE(problems.size() == 1);
        CHECK_THAT(problems[0], ContainsSubstring("too large"));
    }
    SECTION("A stepover that would leave ridges") {
        operation.stepover = 6.0;
        CHECK(pocketMoves(square(40.0), heights(5.0, 0.0), flatMill(6.0), operation, problems)
                  .empty());
        REQUIRE(problems.size() == 1);
        CHECK_THAT(problems[0], ContainsSubstring("stepover"));
    }
    SECTION("A stepdown deeper than the flutes") {
        operation.stepdown = 30.0;
        CHECK(pocketMoves(square(40.0), heights(50.0, 10.0), flatMill(6.0), operation, problems)
                  .empty());
        REQUIRE(problems.size() == 1);
        CHECK_THAT(problems[0], ContainsSubstring("stepdown"));
    }
    SECTION("A pocket deeper than the tool sticks out") {
        Tool stubby = flatMill(6.0);
        stubby.stickout = 10.0;
        CHECK(pocketMoves(square(40.0), heights(30.0, 0.0), stubby, operation, problems).empty());
        REQUIRE(problems.size() == 1);
        CHECK_THAT(problems[0], ContainsSubstring("holder"));
    }
    SECTION("A drill for a pocket") {
        Tool drill = flatMill(6.0);
        drill.kind = ToolKind::Drill;
        CHECK(pocketMoves(square(40.0), heights(5.0, 0.0), drill, operation, problems).empty());
        CHECK_THAT(problems[0], ContainsSubstring("end mill"));
    }
    SECTION("Nothing to cut") {
        CHECK(pocketMoves(square(40.0), heights(5.0, 5.0), flatMill(6.0), operation, problems)
                  .empty());
        CHECK_THAT(problems[0], ContainsSubstring("nothing to cut"));
    }
    SECTION("A wrong drill for a hole") {
        const kernel::RoundFace hole{{0.0, 0.0}, 4.0, -5.0, 5.0};
        Tool drill = flatMill(6.0);
        drill.kind = ToolKind::Drill;
        CHECK(drillMoves({hole}, -5.0, heights(5.0, 0.0), drill, operation, problems).empty());
        CHECK_THAT(problems[0], ContainsSubstring("makes a hole"));
    }
}

TEST_CASE("A contour runs outside its outline by the tool's radius, a climb cut", "[cam]") {
    MachiningOperation operation = cut(OperationKind::Contour);
    operation.side = ContourSide::Outside;
    std::vector<std::string> problems;
    const std::vector<Move> moves =
        contourMoves(square(40.0), heights(10.0, 0.0), flatMill(6.0), operation, problems);
    REQUIRE(problems.empty());
    CHECK_THAT(minOf(moves, &kernel::Point::x), WithinAbs(-3.0, 1e-3));
    CHECK_THAT(maxOf(moves, &kernel::Point::x), WithinAbs(43.0, 1e-3));
    CHECK_THAT(minOf(moves, &kernel::Point::z), WithinAbs(0.0, 1e-9));

    // Around the part clockwise, as seen from above: the signed area of the cut is negative.
    double area = 0.0;
    kernel::Point previous{};
    bool first = true;
    for (const Move& move : moves) {
        if (move.kind == MoveKind::Feed && std::abs(move.to.z) < 1e-9) {
            if (!first) {
                area += previous.x * move.to.y - move.to.x * previous.y;
            }
            first = false;
            previous = move.to;
        }
    }
    CHECK(area < 0.0);

    // Inside, it runs the other way, and a tool's radius inside.
    operation.side = ContourSide::Inside;
    const std::vector<Move> inner =
        contourMoves(square(40.0), heights(10.0, 0.0), flatMill(6.0), operation, problems);
    CHECK_THAT(minOf(inner, &kernel::Point::x), WithinAbs(3.0, 1e-3));
    CHECK_THAT(maxOf(inner, &kernel::Point::x), WithinAbs(37.0, 1e-3));
}

TEST_CASE("Facing covers the whole stock, from outside it", "[cam]") {
    kernel::BoundingBox stock;
    stock.min = {0.0, 0.0, 0.0};
    stock.max = {50.0, 30.0, 10.0};
    std::vector<std::string> problems;
    MachiningOperation operation = cut(OperationKind::Face);
    const std::vector<Move> moves =
        faceMoves(stock, heights(10.0, 9.0), flatMill(6.0), operation, problems);
    REQUIRE(problems.empty());
    // It goes down beside the stock, not on it.
    for (const Move& move : moves) {
        if (move.kind == MoveKind::Plunge) {
            CHECK(move.to.x < stock.min.x - 3.0);
        }
    }
    CHECK(minOf(moves, &kernel::Point::x) < stock.min.x - 3.0);
    CHECK(maxOf(moves, &kernel::Point::x) > stock.max.x + 3.0);
    CHECK_THAT(minOf(moves, &kernel::Point::y), WithinAbs(0.0, 1e-9));
    CHECK_THAT(maxOf(moves, &kernel::Point::y), WithinAbs(30.0, 1e-9));
}

TEST_CASE("Holes are drilled nearest next, through and past, with pecks", "[cam]") {
    Tool drill = flatMill(6.0);
    drill.kind = ToolKind::Drill;
    MachiningOperation operation = cut(OperationKind::Drill);
    operation.peck = 3.0;
    const std::vector<kernel::RoundFace> holes{{{50.0, 0.0}, 3.0, -10.0, 0.0},
                                               {{0.0, 0.0}, 3.0, -10.0, 0.0},
                                               {{10.0, 0.0}, 3.0, -4.0, 0.0}};
    std::vector<std::string> problems;
    const std::vector<Move> moves =
        drillMoves(holes, -10.0, heights(0.0, -10.0), drill, operation, problems);
    REQUIRE(problems.empty());
    // In the order 50, then 10, then 0: the shortest way from the first.
    std::vector<double> order;
    for (const Move& move : moves) {
        if (move.kind == MoveKind::Plunge && (order.empty() || order.back() != move.to.x)) {
            order.push_back(move.to.x);
        }
    }
    CHECK(order == std::vector<double>{50.0, 10.0, 0.0});

    const double tip = 3.0 / std::tan(118.0 * 3.14159265358979323846 / 360.0);
    double throughBottom = 0.0;
    double blindBottom = 0.0;
    int pecks = 0;
    for (const Move& move : moves) {
        if (move.kind == MoveKind::Plunge) {
            ++pecks;
            if (move.to.x == 0.0) {
                throughBottom = std::min(throughBottom, move.to.z);
            } else if (move.to.x == 10.0) {
                blindBottom = std::min(blindBottom, move.to.z);
            }
        }
        // Nothing sideways below the top.
        if (move.kind != MoveKind::Plunge) {
            CHECK((move.kind == MoveKind::Rapid));
        }
    }
    CHECK_THAT(throughBottom, WithinAbs(-10.0 - tip - 0.5, 1e-9));
    CHECK_THAT(blindBottom, WithinAbs(-4.0, 1e-9));
    CHECK(pecks > 6);
}

TEST_CASE("A setup becomes toolpaths from the faces of the model", "[cam]") {
    Part part;
    const Setup setup = part.setup();
    const WorkArea work = workAreaOf(part.document, setup);
    CHECK(work.stock.min.x == -5.0);
    CHECK(work.stock.max.x == 85.0);
    CHECK(work.stock.max.z == 13.0);
    CHECK(work.stock.min.z == -1.0);
    CHECK(work.origin.x == -5.0);
    CHECK(work.origin.y == -5.0);
    CHECK(work.origin.z == 13.0);

    const Program program = planSetup(part.document, setup);
    INFO((program.problems.empty() ? std::string() : program.problems.front().text));
    CHECK_FALSE(program.hasErrors());
    REQUIRE(program.paths.size() == 4);
    CHECK(program.paths[0].name == "Face");
    CHECK_THAT(minOf(program.paths[0].moves, &kernel::Point::z), WithinAbs(12.0, 1e-9));
    // The pocket's floor, a tool's radius in from its walls.
    CHECK_THAT(minOf(program.paths[1].moves, &kernel::Point::z), WithinAbs(6.0, 1e-9));
    CHECK_THAT(minOf(program.paths[1].moves, &kernel::Point::x), WithinAbs(13.0, 1e-3));
    // The outside, down to the bottom of the part.
    CHECK_THAT(minOf(program.paths[2].moves, &kernel::Point::z), WithinAbs(0.0, 1e-9));
    CHECK_THAT(minOf(program.paths[2].moves, &kernel::Point::x), WithinAbs(-3.0, 1e-3));
    // The hole, right through.
    CHECK(minOf(program.paths[3].moves, &kernel::Point::z) < 0.0);
    CHECK_THAT(maxOf(program.paths[3].moves, &kernel::Point::x), WithinAbs(65.0, 1e-9));

    SECTION("An operation that has lost its face says so, and the others stand") {
        Setup changed = setup;
        changed.operations[1].targets[0].face = "gone";
        const Program broken = planSetup(part.document, changed);
        CHECK(broken.hasErrors());
        CHECK(broken.paths.size() == 3);
        REQUIRE(broken.problems.size() == 1);
        CHECK(broken.problems[0].operation == changed.operations[1].id);
        CHECK_THAT(broken.problems[0].text, ContainsSubstring("not in the model"));
    }
    SECTION("What the machine cannot do is refused") {
        Setup changed = setup;
        changed.machine.maxSpindle = 1000.0;
        CHECK(planSetup(part.document, changed).hasErrors());
        changed = setup;
        changed.machine.travelX = 50.0;
        const Program tooWide = planSetup(part.document, changed);
        REQUIRE(tooWide.hasErrors());
        CHECK_THAT(tooWide.problems.back().text, ContainsSubstring("travels"));
        changed = setup;
        changed.operations[0].feed = 0.0;
        CHECK(planSetup(part.document, changed).hasErrors());
        changed = setup;
        changed.operations[0].tool = ObjectId{9999};
        CHECK(planSetup(part.document, changed).hasErrors());
    }
    SECTION("A suppressed operation is left out") {
        Setup changed = setup;
        changed.operations[0].suppressed = true;
        CHECK(planSetup(part.document, changed).paths.size() == 3);
    }
}

TEST_CASE("The whole setup cuts the part out of the stock, and nothing of the part",
          "[cam][simulation]") {
    Part part;
    const Program program = planSetup(part.document, part.setup());
    REQUIRE_FALSE(program.hasErrors());
    Simulation simulation(program, part.mesh, Setup{}.machine, 0.2);
    const SimulationReport report = simulation.run();
    INFO((report.problems.empty() ? std::string() : report.problems.front().text));
    CHECK_FALSE(report.hasErrors());
    CHECK(report.cuttingSeconds > 0.0);
    CHECK(report.rapidSeconds > 0.0);
    CHECK(report.cuttingLength > report.rapidLength / 10.0);
    CHECK(report.removedVolume > 0.0);

    const HeightField& stock = simulation.stock();
    const auto height = [&stock](double x, double y) {
        return static_cast<double>(stock.sample(x, y, 1e6F));
    };
    CHECK_THAT(height(5.0, 5.0), WithinAbs(12.0, 0.01));  // the top, faced
    CHECK_THAT(height(30.0, 30.0), WithinAbs(6.0, 0.01)); // the pocket's floor
    CHECK(height(-2.0, 30.0) <= 0.0 + 0.01);              // the contour's trench
    CHECK(height(65.0, 30.0) < 0.0);                      // the hole
    // What is left over the part's surfaces is no more than a tool leaves in a sharp corner.
    CHECK(report.leftAbovePart <= 6.0 + 0.01);

    // And the time adds up: at the least the cut's length at its fastest.
    CHECK(report.cuttingSeconds >= report.cuttingLength / 600.0 * 60.0 - 1e-6);
}

TEST_CASE("The simulation catches what would go wrong", "[cam][simulation]") {
    Part part;
    Program program = planSetup(part.document, part.setup());
    REQUIRE(program.paths.size() == 4);
    const Machine machine;

    SECTION("A cut that goes too deep") {
        Move& deepest =
            *std::min_element(program.paths[1].moves.begin(), program.paths[1].moves.end(),
                              [](const Move& a, const Move& b) { return a.to.z < b.to.z; });
        deepest.to.z = 3.0;
        Simulation simulation(program, part.mesh, machine, 0.25);
        const SimulationReport report = simulation.run();
        REQUIRE(report.hasErrors());
        CHECK_THAT(report.problems.front().text, ContainsSubstring("cuts into the part"));
        CHECK(report.problems.front().operation == program.paths[1].operation);
    }
    SECTION("A rapid move through the material") {
        std::vector<Move>& moves = program.paths[1].moves;
        moves.insert(moves.begin() + 3, {MoveKind::Rapid, {40.0, 30.0, 8.0}, 0.0});
        Simulation simulation(program, part.mesh, machine, 0.25);
        const SimulationReport report = simulation.run();
        REQUIRE(report.hasErrors());
        CHECK_THAT(report.problems.front().text, ContainsSubstring("rapid move"));
    }
    SECTION("A tool buried past its flutes") {
        program.paths[2].tool.fluteLength = 1.0;
        Simulation simulation(program, part.mesh, machine, 0.25);
        const SimulationReport report = simulation.run();
        REQUIRE(report.hasErrors());
        CHECK_THAT(report.problems.front().text, ContainsSubstring("holder"));
    }
    SECTION("Moves can be run a few at a time, and end the same") {
        Simulation stepped(program, part.mesh, machine, 0.5);
        CHECK(stepped.moveCount() > 100);
        stepped.runTo(50);
        CHECK(stepped.movesDone() == 50);
        CHECK(stepped.tool() != nullptr);
        const auto at = stepped.toolPosition();
        stepped.runTo(40);
        CHECK(stepped.movesDone() == 50);
        CHECK(stepped.toolPosition().x == at.x);
        const SimulationReport report = stepped.run();
        Simulation whole(program, part.mesh, machine, 0.5);
        CHECK_THAT(report.removedVolume, WithinAbs(whole.run().removedVolume, 1e-3));
        // Its surface is one vertex a cell.
        const kernel::Mesh mesh = stepped.stockMesh(4);
        CHECK_FALSE(mesh.triangles.empty());
    }
}

TEST_CASE("Each dialect gets what it has, and reads back as the same moves", "[cam][gcode]") {
    Part part;
    Setup setup = part.setup();
    for (const PostDialect dialect : {PostDialect::Grbl, PostDialect::LinuxCnc}) {
        setup.machine.dialect = dialect;
        const Program program = planSetup(part.document, setup);
        REQUIRE_FALSE(program.hasErrors());
        const std::string text = writeGcode(program, setup.machine, "bracket (rev A)");

        const std::vector<std::string> errors = verifyGcode(text, program, dialect);
        INFO((errors.empty() ? std::string() : errors.front()));
        CHECK(errors.empty());

        const GcodeReading reading = readGcode(text, dialect);
        CHECK(reading.errors.empty());
        CHECK(reading.spindle == 12000.0);
        CHECK(reading.feed == 600.0);
        CHECK(reading.moves.size() == machineMoves(program).size());

        // Zero is at the stock's top corner: the machine goes down to minus, and not below
        // the bottom of the stock but for the drill.
        double lowest = 0.0;
        for (const Move& move : reading.moves) {
            lowest = std::min(lowest, move.to.z);
        }
        CHECK(lowest < -12.0);
        CHECK(lowest > -18.0);

        // Comments cannot be broken by what is in a name.
        CHECK_THAT(text, ContainsSubstring("(LCAD: bracket  rev A )"));
        CHECK(text.rfind("(", 0) == 0);

        if (dialect == PostDialect::Grbl) {
            // No tool changer, no blending, and a stop for the operator between the tools.
            CHECK(text.find("M6") == std::string::npos);
            CHECK(text.find("G43") == std::string::npos);
            CHECK(text.find("G64") == std::string::npos);
            CHECK_THAT(text, ContainsSubstring("M0\n"));
            CHECK_THAT(text, ContainsSubstring("M30\n"));
        } else {
            CHECK_THAT(text, ContainsSubstring("T1 M6\n"));
            CHECK_THAT(text, ContainsSubstring("T2 M6\n"));
            CHECK_THAT(text, ContainsSubstring("G43 H2\n"));
            CHECK_THAT(text, ContainsSubstring("G64 P0.01\n"));
            CHECK_THAT(text, ContainsSubstring("M2\n"));
        }
    }
}

TEST_CASE("G-code starts safely: up first, spindle on, nothing else", "[cam][gcode]") {
    Part part;
    const Setup setup = part.setup();
    const Program program = planSetup(part.document, setup);
    const std::string text = writeGcode(program, setup.machine, "part");
    std::istringstream lines(text);
    std::string line;
    std::vector<std::string> code;
    while (std::getline(lines, line)) {
        if (line.empty() || line[0] != '(') {
            code.push_back(line);
        }
    }
    REQUIRE(code.size() > 8);
    CHECK(code[0] == "G21 G90 G17 G94");
    CHECK(code[1] == "G54");
    CHECK(code[2].rfind("G0 Z", 0) == 0);
    CHECK(code[3].rfind("S12000 M3", 0) == 0);
    CHECK(code[4] == "G4 P2");
    CHECK(code[5].find('X') != std::string::npos);
    CHECK(code[5].find('Z') == std::string::npos);
    CHECK(code[code.size() - 2] == "M5");
    CHECK(code.back() == "M30");
}

TEST_CASE("The reader refuses what the machine would refuse or get wrong", "[cam][gcode]") {
    const auto errorsOf = [](const std::string& text, PostDialect dialect = PostDialect::Grbl) {
        return readGcode(text, dialect).errors;
    };
    const std::string start = "G21 G90 G94\nG0 Z5\nG0 X0 Y0\nS1000 M3\n";

    CHECK(errorsOf(start + "G1 Z-1 F100\nM5\nM30\n").empty());

    const auto firstError = [&](const std::string& text, PostDialect dialect = PostDialect::Grbl) {
        const auto errors = errorsOf(text, dialect);
        REQUIRE_FALSE(errors.empty());
        return errors.front();
    };
    CHECK_THAT(firstError("G0 Z5\n"), ContainsSubstring("G21"));
    CHECK_THAT(firstError("G21 G94\nG0 Z5\n"), ContainsSubstring("G90"));
    CHECK_THAT(firstError("G21 G90 G94\nG0 X1 Y1\n"), ContainsSubstring("height"));
    CHECK_THAT(firstError("G21 G90 G94\nG0 Z5\nG0 X0 Y0\nG1 Z-1 F100\n"),
               ContainsSubstring("spindle"));
    CHECK_THAT(firstError(start + "G1 Z-1\n"), ContainsSubstring("no feed"));
    CHECK_THAT(firstError(start + "M6\n"), ContainsSubstring("no M6"));
    CHECK_THAT(firstError(start + "T2\n"), ContainsSubstring("no T"));
    CHECK_THAT(firstError(start + "G64 P0.01\n"), ContainsSubstring("no G64"));
    CHECK_THAT(firstError(start + "G83 Z-1\n"), ContainsSubstring("no G83"));
    CHECK_THAT(firstError(start + "(comment\n"), ContainsSubstring("not closed"));
    CHECK_THAT(firstError(start + std::string(90, 'G') + "\n"), ContainsSubstring("too long"));
    CHECK_THAT(firstError(start + "@\n"), ContainsSubstring("not a word"));
    CHECK_THAT(firstError(start + "G1 X\n"), ContainsSubstring("no number"));
    CHECK(errorsOf(start + "T2 M6\nG43 H2\n", PostDialect::LinuxCnc).empty());
}

TEST_CASE("G-code has a point in its numbers, whatever language the system speaks",
          "[cam][gcode]") {
    Part part;
    const Setup setup = part.setup();
    const Program program = planSetup(part.document, setup);
    const std::string expected = writeGcode(program, setup.machine, "part");

    // Qt sets the language of the system for the C library when an application starts.
    const std::string before = std::setlocale(LC_ALL, nullptr);
    bool tried = false;
    for (const char* name : {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "de_DE"}) {
        if (std::setlocale(LC_ALL, name) != nullptr) {
            tried = true;
            CHECK(writeGcode(program, setup.machine, "part") == expected);
        }
    }
    std::setlocale(LC_ALL, before.c_str());
    if (!tried) {
        WARN("No language with a decimal comma is installed here.");
    }
    // In code, as opposed to comments, there is no comma at all.
    std::istringstream lines(expected);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line[0] != '(') {
            CHECK(line.find(',') == std::string::npos);
        }
    }
}

TEST_CASE("Verification catches G-code that differs from its program", "[cam][gcode]") {
    Part part;
    const Setup setup = part.setup();
    const Program program = planSetup(part.document, setup);
    const std::string text = writeGcode(program, setup.machine, "part");
    REQUIRE(verifyGcode(text, program, PostDialect::Grbl).empty());

    // A number altered by a hundredth of a millimeter.
    std::string changed = text;
    const auto line = changed.find("G1 X");
    REQUIRE(line != std::string::npos);
    const auto digit = changed.find_first_of("0123456789", line + 4);
    changed[digit] = changed[digit] == '9' ? '8' : static_cast<char>(changed[digit] + 1);
    CHECK_FALSE(verifyGcode(changed, program, PostDialect::Grbl).empty());

    // A line lost.
    std::string shorter = text;
    shorter.erase(line, shorter.find('\n', line) - line + 1);
    CHECK_FALSE(verifyGcode(shorter, program, PostDialect::Grbl).empty());

    // A cut turned into a rapid.
    std::string rapid = text;
    rapid[line + 1] = '0';
    CHECK_FALSE(verifyGcode(rapid, program, PostDialect::Grbl).empty());
}
