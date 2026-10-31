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
#include <limits>
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
    // Three depths, with the tool going down at the plunge feed.
    std::vector<double> levels;
    for (const Move& move : moves) {
        if (move.kind == MoveKind::Plunge) {
            CHECK(move.feed == 200.0);
        } else if (move.kind == MoveKind::Feed && move.to.z < 10.0) {
            levels.push_back(move.to.z);
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

namespace {

// A program of one path of moves, on a block.
Program programOf(const kernel::BoundingBox& stock, const Tool& tool, std::vector<Move> moves) {
    Program program;
    program.work.stock = stock;
    program.work.part = stock;
    program.safeZ = stock.max.z + 5.0;
    Toolpath path;
    path.name = "Cut";
    path.tool = tool;
    path.spindle = 12000.0;
    path.moves = std::move(moves);
    program.paths.push_back(std::move(path));
    return program;
}

// The steepest that moves go down into material no move has reached yet, from a height where it
// starts, as height for distance across. Straight down has no distance across, and comes to
// infinity.
double steepestInto(const std::vector<Move>& moves, double height) {
    double steepest = 0.0;
    double reached = height;
    kernel::Point at = moves.front().to;
    for (std::size_t i = 1; i < moves.size(); ++i) {
        const kernel::Point& to = moves[i].to;
        if (moves[i].kind != MoveKind::Rapid && to.z < reached - 1e-9) {
            const double across = std::hypot(to.x - at.x, to.y - at.y);
            steepest = std::max(steepest, across > 1e-9 ? (at.z - to.z) / across
                                                        : std::numeric_limits<double>::infinity());
            reached = to.z;
        }
        at = to;
    }
    return steepest;
}

// How much of a line across a stock stands higher than `level`, a stretch at a time.
std::vector<double> standing(const HeightField& stock, kernel::Point2D from, kernel::Point2D to,
                             double level) {
    std::vector<double> stretches;
    const double length = std::hypot(to.x - from.x, to.y - from.y);
    const double step = 0.05;
    double run = 0.0;
    for (double along = 0.0; along <= length; along += step) {
        const double x = from.x + (to.x - from.x) * along / length;
        const double y = from.y + (to.y - from.y) * along / length;
        if (static_cast<double>(stock.sample(x, y, 0.0F)) > level) {
            run += step;
        } else if (run > 0.0) {
            stretches.push_back(run);
            run = 0.0;
        }
    }
    if (run > 0.0) {
        stretches.push_back(run);
    }
    return stretches;
}

kernel::FlatRegion round(double radius, int sides) {
    kernel::FlatRegion region;
    for (int i = 0; i < sides; ++i) {
        const double angle = 2.0 * 3.14159265358979 * i / sides;
        region.outer.push_back({radius * std::cos(angle), radius * std::sin(angle)});
    }
    return region;
}

} // namespace

TEST_CASE("A cut goes down along its path, not straight in", "[cam]") {
    const Tool tool = flatMill(6.0);
    const double slope = std::tan(3.0 * 3.14159265358979 / 180.0);
    std::vector<std::string> problems;

    SECTION("A pocket, flat at the bottom when it is done") {
        MachiningOperation operation = cut(OperationKind::Pocket);
        const auto moves = pocketMoves(square(40.0), heights(10.0, 4.0), tool, operation, problems);
        REQUIRE(problems.empty());
        CHECK(steepestInto(moves, 10.0) <= slope * 1.001);

        Program program = programOf({{-5.0, -5.0, 0.0}, {45.0, 45.0, 10.0}}, tool, moves);
        Simulation simulation(program, {}, Setup{}.machine, 0.1);
        CHECK_FALSE(simulation.run().hasErrors());
        for (const double x : {3.5, 12.0, 20.0, 28.0, 36.5}) {
            for (const double y : {3.5, 20.0, 36.5}) {
                CAPTURE(x, y);
                CHECK_THAT(static_cast<double>(simulation.stock().sample(x, y, 1e6F)),
                           WithinAbs(4.0, 1e-3));
            }
        }
        CHECK_THAT(static_cast<double>(simulation.stock().sample(-1.0, 20.0, 1e6F)),
                   WithinAbs(10.0, 1e-3));
    }
    SECTION("A pocket with only a small round ring to come down around") {
        MachiningOperation operation = cut(OperationKind::Pocket);
        const auto moves =
            pocketMoves(round(5.0, 72), heights(10.0, 4.0), tool, operation, problems);
        REQUIRE(problems.empty());
        CHECK(steepestInto(moves, 10.0) <= slope * 1.001);
        double lowest = 10.0;
        for (const Move& move : moves) {
            lowest = std::min(lowest, move.to.z);
        }
        CHECK_THAT(lowest, WithinAbs(4.0, 1e-9));
    }
    SECTION("A ring too small to go around is gone straight down into") {
        MachiningOperation operation = cut(OperationKind::Pocket);
        const auto moves =
            pocketMoves(round(3.2, 72), heights(10.0, 8.0), tool, operation, problems);
        REQUIRE(problems.empty());
        CHECK(std::isinf(steepestInto(moves, 10.0)));
    }
    SECTION("A contour, climbing all the way") {
        MachiningOperation operation = cut(OperationKind::Contour);
        const auto moves =
            contourMoves(square(40.0), heights(10.0, 0.0), tool, operation, problems);
        REQUIRE(problems.empty());
        CHECK(steepestInto(moves, 10.0) <= slope * 1.001);
        Program program = programOf({{-10.0, -10.0, 0.0}, {50.0, 50.0, 10.0}}, tool, moves);
        Simulation simulation(program, {}, Setup{}.machine, 0.1);
        CHECK_FALSE(simulation.run().hasErrors());
        // Clear down to the bottom all the way around, the ramps left behind.
        CHECK(standing(simulation.stock(), {0.0, -3.0}, {40.0, -3.0}, 0.01).empty());
        CHECK(standing(simulation.stock(), {43.0, 0.0}, {43.0, 40.0}, 0.01).empty());
        CHECK(standing(simulation.stock(), {40.0, 43.0}, {0.0, 43.0}, 0.01).empty());
        CHECK(standing(simulation.stock(), {-3.0, 40.0}, {-3.0, 0.0}, 0.01).empty());
        CHECK_THAT(static_cast<double>(simulation.stock().sample(20.0, 20.0, 0.0F)),
                   WithinAbs(10.0, 1e-3));
    }
    SECTION("No ramp goes straight down") {
        MachiningOperation operation = cut(OperationKind::Contour);
        operation.rampAngle = 0.0;
        const auto moves =
            contourMoves(square(40.0), heights(10.0, 6.0), tool, operation, problems);
        REQUIRE(problems.empty());
        CHECK(std::isinf(steepestInto(moves, 10.0)));
    }
    SECTION("A ramp too steep is refused") {
        MachiningOperation operation = cut(OperationKind::Pocket);
        operation.rampAngle = 46.0;
        CHECK(pocketMoves(square(40.0), heights(10.0, 6.0), tool, operation, problems).empty());
        REQUIRE(problems.size() == 1);
        CHECK_THAT(problems.front(), ContainsSubstring("45"));
    }
}

TEST_CASE("Tabs hold what a contour cuts out", "[cam][simulation]") {
    const Tool tool = flatMill(6.0);
    std::vector<std::string> problems;
    MachiningOperation operation = cut(OperationKind::Contour);
    operation.tabs = 4;
    operation.tabWidth = 6.0;
    operation.tabHeight = 1.5;

    const auto tabsAround = [](const HeightField& stock, double level) {
        std::vector<double> all;
        for (const auto& [from, to] : std::vector<std::pair<kernel::Point2D, kernel::Point2D>>{
                 {{0.0, -3.0}, {40.0, -3.0}},
                 {{43.0, 0.0}, {43.0, 40.0}},
                 {{40.0, 43.0}, {0.0, 43.0}},
                 {{-3.0, 40.0}, {-3.0, 0.0}}}) {
            const std::vector<double> found = standing(stock, from, to, level);
            all.insert(all.end(), found.begin(), found.end());
        }
        return all;
    };

    for (const double ramp : {3.0, 0.0}) {
        CAPTURE(ramp);
        operation.rampAngle = ramp;
        const auto moves =
            contourMoves(square(40.0), heights(10.0, 0.0), tool, operation, problems);
        REQUIRE(problems.empty());
        Program program = programOf({{-10.0, -10.0, 0.0}, {50.0, 50.0, 10.0}}, tool, moves);
        Simulation simulation(program, {}, Setup{}.machine, 0.1);
        CHECK_FALSE(simulation.run().hasErrors());

        // Four bridges, each as wide as asked at the top, sloping away on the far side where the
        // tool comes back down, as high as asked, and no higher.
        const std::vector<double> bridges = tabsAround(simulation.stock(), 1.45);
        REQUIRE(bridges.size() == 4);
        for (const double width : bridges) {
            CHECK_THAT(width, WithinAbs(6.05, 0.2));
        }
        for (const double width : tabsAround(simulation.stock(), 0.01)) {
            CHECK_THAT(width, WithinAbs(7.5, 0.2));
        }
        CHECK(tabsAround(simulation.stock(), 1.5 + 0.01).empty());
        // Everything else around the part is cut through, and the part is untouched.
        CHECK_THAT(static_cast<double>(simulation.stock().sample(20.0, 20.0, 0.0F)),
                   WithinAbs(10.0, 1e-3));
    }

    SECTION("They go on a curve where nothing is straight") {
        operation.rampAngle = 3.0;
        const auto moves =
            contourMoves(round(20.0, 180), heights(10.0, 0.0), tool, operation, problems);
        REQUIRE(problems.empty());
        Program program = programOf({{-30.0, -30.0, 0.0}, {30.0, 30.0, 10.0}}, tool, moves);
        Simulation simulation(program, {}, Setup{}.machine, 0.1);
        CHECK_FALSE(simulation.run().hasErrors());
        int bridges = 0;
        bool over = false;
        for (double angle = 0.0; angle < 6.2831; angle += 0.002) {
            const auto high =
                simulation.stock().sample(23.0 * std::cos(angle), 23.0 * std::sin(angle), 0.0F);
            const bool now = high > 0.5F;
            bridges += now && !over ? 1 : 0;
            over = now;
        }
        CHECK(bridges == 4);
    }
    SECTION("More than fit are refused") {
        operation.tabs = 40;
        CHECK(contourMoves(square(40.0), heights(10.0, 0.0), tool, operation, problems).empty());
        REQUIRE(problems.size() == 1);
        CHECK_THAT(problems.front(), ContainsSubstring("40 tabs do not fit"));
    }
    SECTION("So are tabs as high as the cut is deep, or with no size") {
        operation.tabHeight = 10.0;
        CHECK(contourMoves(square(40.0), heights(10.0, 0.0), tool, operation, problems).empty());
        REQUIRE(problems.size() == 1);
        CHECK_THAT(problems.front(), ContainsSubstring("as high as the cut"));
        problems.clear();
        operation.tabHeight = 1.5;
        operation.tabWidth = 0.0;
        CHECK(contourMoves(square(40.0), heights(10.0, 0.0), tool, operation, problems).empty());
        CHECK(problems.size() == 1);
    }
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
    // The outside goes right through, and has no tabs; the drill's point goes past the stock,
    // into what it stands on. Both are worth knowing, and no reason to stop.
    REQUIRE(program.problems.size() == 2);
    CHECK(program.problems[0].severity == Problem::Severity::Warning);
    CHECK(program.problems[0].operation == setup.operations[2].id);
    CHECK_THAT(program.problems[0].text, ContainsSubstring("comes loose"));
    CHECK(program.problems[1].severity == Problem::Severity::Warning);
    CHECK(program.problems[1].operation == setup.operations[3].id);
    CHECK_THAT(program.problems[1].text, ContainsSubstring("below the stock"));
    {
        Setup held = setup;
        held.operations[2].tabs = 4;
        const Program tabbed = planSetup(part.document, held);
        REQUIRE(tabbed.problems.size() == 1);
        CHECK_THAT(tabbed.problems[0].text, ContainsSubstring("below the stock"));
    }

    SECTION("An operation that has lost its face says so, and the others stand") {
        Setup changed = setup;
        changed.operations[1].targets[0].face = "gone";
        const Program broken = planSetup(part.document, changed);
        CHECK(broken.hasErrors());
        CHECK(broken.paths.size() == 3);
        REQUIRE(broken.problems.size() == 3);
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

TEST_CASE("A cut takes away just what the tool sweeps, at any angle", "[cam][simulation]") {
    const auto groove = [](const Tool& tool, const kernel::Point& end) {
        Program program;
        program.work.stock = {{0.0, 0.0, 0.0}, {100.0, 100.0, 10.0}};
        program.safeZ = 20.0;
        Toolpath path;
        path.name = "Groove";
        path.tool = tool;
        path.moves = {{MoveKind::Rapid, {20.0, 20.0, 20.0}, 0.0},
                      {MoveKind::Plunge, {20.0, 20.0, 5.0}, 100.0},
                      {MoveKind::Feed, end, 500.0},
                      {MoveKind::Rapid, {end.x, end.y, 20.0}, 0.0}};
        program.paths.push_back(path);
        return program;
    };
    const auto machine = Setup{}.machine;

    SECTION("A flat mill leaves a slot with round ends and a flat floor") {
        const Program program = groove(flatMill(6.0), {80.0, 60.0, 5.0});
        Simulation simulation(program, {}, machine, 0.1);
        const SimulationReport report = simulation.run();
        CHECK_FALSE(report.hasErrors());
        const double length = std::hypot(60.0, 40.0);
        const double area = 6.0 * length + 3.14159265358979 * 9.0;
        CHECK_THAT(report.removedVolume, WithinAbs(area * 5.0, area * 5.0 * 0.01));

        // All along it, just inside its walls and just outside them.
        const double acrossX = -40.0 / length;
        const double acrossY = 60.0 / length;
        for (int i = 0; i <= 40; ++i) {
            const double x = 20.0 + 60.0 * i / 40.0;
            const double y = 20.0 + 40.0 * i / 40.0;
            CAPTURE(i);
            CHECK_THAT(static_cast<double>(simulation.stock().sample(x, y, 1e6F)),
                       WithinAbs(5.0, 1e-3));
            for (const double side : {-1.0, 1.0}) {
                const double inX = x + side * 2.85 * acrossX;
                const double inY = y + side * 2.85 * acrossY;
                CHECK_THAT(static_cast<double>(simulation.stock().sample(inX, inY, 1e6F)),
                           WithinAbs(5.0, 1e-3));
                const double outX = x + side * 3.15 * acrossX;
                const double outY = y + side * 3.15 * acrossY;
                CHECK_THAT(static_cast<double>(simulation.stock().sample(outX, outY, 1e6F)),
                           WithinAbs(10.0, 1e-3));
            }
        }
    }
    SECTION("A ball mill leaves a round floor") {
        Tool ball = flatMill(6.0);
        ball.kind = ToolKind::BallEndMill;
        const Program program = groove(ball, {80.0, 20.0, 5.0});
        Simulation simulation(program, {}, machine, 0.1);
        simulation.run();
        CHECK_THAT(static_cast<double>(simulation.stock().sample(50.0, 20.05, 1e6F)),
                   WithinAbs(5.0, 0.01));
        // Two millimeters to the side of the middle, near enough: cells are 0.1 across.
        const double side = simulation.stock().y(simulation.stock().rowAt(22.0)) - 20.0;
        CHECK_THAT(static_cast<double>(simulation.stock().sample(50.0, 22.0, 1e6F)),
                   WithinAbs(5.0 + 3.0 - std::sqrt(9.0 - side * side), 1e-3));
    }
    SECTION("A slanted cut goes down as it goes along") {
        const Program program = groove(flatMill(6.0), {80.0, 20.0, 2.0});
        Simulation simulation(program, {}, machine, 0.1);
        simulation.run();
        CHECK_THAT(static_cast<double>(simulation.stock().sample(50.0, 20.05, 1e6F)),
                   WithinAbs(3.5 - 3.0 * 3.0 / 60.0, 0.03));
        CHECK_THAT(static_cast<double>(simulation.stock().sample(79.0, 20.05, 1e6F)),
                   WithinAbs(2.0, 0.03));
    }
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
        // Fewer than the program, which has its curves in many short lines.
        CHECK(reading.moves.size() < machineMoves(program).size());
        CHECK_FALSE(reading.arcs.empty());

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

TEST_CASE("Curves are written as arcs, and read back along the moves", "[cam][gcode]") {
    const Tool tool = flatMill(6.0);
    std::vector<std::string> problems;
    MachiningOperation operation = cut(OperationKind::Contour);
    operation.rampAngle = 0.0;
    Machine machine;
    const kernel::BoundingBox stock{{-30.0, -30.0, 0.0}, {70.0, 70.0, 10.0}};
    const auto lines = [](const std::string& text, const char* start) {
        int found = 0;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            found += line.rfind(start, 0) == 0 ? 1 : 0;
        }
        return found;
    };
    const auto wholeProgram = [&](const kernel::FlatRegion& region) {
        const auto moves = contourMoves(region, heights(10.0, 6.0), tool, operation, problems);
        REQUIRE(problems.empty());
        return programOf(stock, tool, moves);
    };

    SECTION("The corners of a square, going the way the tool does") {
        const Program program = wholeProgram(square(40.0));
        for (const PostDialect dialect : {PostDialect::Grbl, PostDialect::LinuxCnc}) {
            machine.dialect = dialect;
            const std::string text = writeGcode(program, machine, "corners");
            const auto errors = verifyGcode(text, program, dialect);
            INFO((errors.empty() ? std::string() : errors.front()));
            CHECK(errors.empty());
            // Two passes of four corners, clockwise: outside, a climb cut.
            CHECK(lines(text, "G2 ") == 8);
            CHECK(lines(text, "G3 ") == 0);
            const GcodeReading reading = readGcode(text, dialect);
            CHECK(reading.errors.empty());
            REQUIRE(reading.arcs.size() == 8);
            // Around the corners of the square, the tool's radius from them.
            for (const GcodeArc& arc : reading.arcs) {
                const double x = std::round(arc.centerX / 40.0) * 40.0;
                const double y = std::round(arc.centerY / 40.0) * 40.0;
                CHECK(std::hypot(arc.centerX - x, arc.centerY - y) < 0.01);
                const Move& move = reading.moves[arc.move];
                CHECK_THAT(std::hypot(move.to.x - arc.centerX, move.to.y - arc.centerY),
                           WithinAbs(3.0, 0.01));
            }
        }
    }
    SECTION("Inside an outline they go the other way") {
        operation.side = ContourSide::Inside;
        kernel::FlatRegion rounded = round(30.0, 720);
        const Program program = wholeProgram(rounded);
        const std::string text = writeGcode(program, machine, "inside");
        CHECK(verifyGcode(text, program, machine.dialect).empty());
        CHECK(lines(text, "G3 ") > 0);
        CHECK(lines(text, "G2 ") == 0);
    }
    SECTION("A whole circle takes several, each short of half a turn") {
        const Program program = wholeProgram(round(20.0, 720));
        const std::string text = writeGcode(program, machine, "circle");
        const auto errors = verifyGcode(text, program, machine.dialect);
        INFO((errors.empty() ? std::string() : errors.front()));
        CHECK(errors.empty());
        const GcodeReading reading = readGcode(text, machine.dialect);
        CHECK(reading.arcs.size() >= 6);
        kernel::Point from;
        std::size_t next = 0;
        for (std::size_t i = 0; i < reading.moves.size(); ++i) {
            const kernel::Point& to = reading.moves[i].to;
            if (next < reading.arcs.size() && reading.arcs[next].move == i) {
                const double cx = reading.arcs[next].centerX;
                const double cy = reading.arcs[next].centerY;
                const double a = std::atan2(from.y - cy, from.x - cx);
                const double b = std::atan2(to.y - cy, to.x - cx);
                const double sweep = std::abs(std::remainder(a - b, 6.28318530717959));
                CHECK(sweep < 3.0);
                ++next;
            }
            from = to;
        }
    }
    SECTION("Ramps and tabs stay lines, and the level cuts between them can be arcs") {
        operation.rampAngle = 3.0;
        operation.tabs = 4;
        const auto moves =
            contourMoves(round(30.0, 720), heights(10.0, 0.0), tool, operation, problems);
        REQUIRE(problems.empty());
        const Program program = programOf(stock, tool, moves);
        const std::string text = writeGcode(program, machine, "tabs");
        const auto errors = verifyGcode(text, program, machine.dialect);
        INFO((errors.empty() ? std::string() : errors.front()));
        CHECK(errors.empty());
        CHECK(lines(text, "G2 ") > 0);
        // A level arc says nothing of height.
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("G2 ", 0) == 0) {
                CHECK(line.find('Z') == std::string::npos);
            }
        }
    }
    SECTION("Machines can be told to have only lines") {
        machine.arcs = false;
        const Program program = wholeProgram(square(40.0));
        const std::string text = writeGcode(program, machine, "lines");
        CHECK(verifyGcode(text, program, machine.dialect).empty());
        CHECK(lines(text, "G2 ") == 0);
        CHECK(lines(text, "G3 ") == 0);
        CHECK(readGcode(text, machine.dialect).moves.size() == machineMoves(program).size());
    }
    SECTION("Verification catches an arc that is not what the program does") {
        const Program program = wholeProgram(square(40.0));
        const std::string text = writeGcode(program, machine, "corners");
        REQUIRE(verifyGcode(text, program, machine.dialect).empty());
        const std::size_t at = text.find("\nG2 ") + 1;
        const std::size_t end = text.find('\n', at);
        const std::string arc = text.substr(at, end - at);

        // The other way around.
        std::string wrong = text;
        wrong[at + 1] = '3';
        CHECK_FALSE(verifyGcode(wrong, program, machine.dialect).empty());

        // Around somewhere else, though it starts and ends in the same places: mirrored about
        // the line between them, which gives the same radius.
        const auto word = [&arc](char letter) {
            const std::size_t from = arc.find(letter) + 1;
            return std::stod(arc.substr(from, arc.find(' ', from) - from));
        };
        const auto replaced = [&](const std::string& line) {
            return text.substr(0, at) + line + text.substr(end);
        };
        std::string bare = arc.substr(0, arc.find(" I"));
        const std::string after =
            arc.find(" F") == std::string::npos ? std::string() : arc.substr(arc.find(" F"));
        const double i = word('I');
        const double j = word('J');
        const kernel::Point lastPlace =
            readGcode(text.substr(0, at), machine.dialect).moves.back().to;
        const double chordX = word('X') - lastPlace.x;
        const double chordY = word('Y') - lastPlace.y;
        const double mirroredI = chordX - i;
        const double mirroredJ = chordY - j;
        std::ostringstream mirrored;
        mirrored.imbue(std::locale::classic());
        mirrored.precision(4);
        mirrored << std::fixed << bare << " I" << mirroredI << " J" << mirroredJ << after;
        CHECK_FALSE(verifyGcode(replaced(mirrored.str()), program, machine.dialect).empty());

        // A center that is off, which the machine itself would refuse.
        std::ostringstream off;
        off.imbue(std::locale::classic());
        off.precision(4);
        off << std::fixed << bare << " I" << (i + 0.5) << " J" << j << after;
        const auto reading = readGcode(replaced(off.str()), machine.dialect);
        REQUIRE_FALSE(reading.errors.empty());
        CHECK_THAT(reading.errors.front(), ContainsSubstring("does not end on its circle"));

        // An arc gone.
        CHECK_FALSE(verifyGcode(replaced("(gone)"), program, machine.dialect).empty());
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

    // Arcs: in a plane it has been told of, around a center, ending on the circle.
    const std::string plane = "G21 G90 G94 G17\nG0 Z5\nG0 X0 Y0\nS1000 M3\nG1 Z-1 F100\n";
    CHECK(errorsOf(plane + "G2 X10 Y0 I5 J0\nM5\nM30\n").empty());
    CHECK(errorsOf(plane + "G3 X10 Y0 I5\nM5\nM30\n", PostDialect::LinuxCnc).empty());
    CHECK_THAT(firstError(start + "G1 Z-1 F100\nG2 X10 Y0 I5\n"), ContainsSubstring("G17"));
    CHECK_THAT(firstError(plane + "G18\nG2 X10 Y0 I5\n"), ContainsSubstring("G17"));
    CHECK_THAT(firstError(plane + "G2 X10 Y0\n"), ContainsSubstring("no center"));
    CHECK_THAT(firstError(plane + "G2 X10 Y0 I3\n"), ContainsSubstring("end on its circle"));
    CHECK_THAT(firstError(plane + "G1 X10 I3\n"), ContainsSubstring("not an arc"));
    CHECK_THAT(firstError(plane + "G2 X10 Y0 R5\n"), ContainsSubstring("no R"));
    CHECK_THAT(firstError("G21 G90 G94 G17\nG0 Z5\nG0 X0 Y0\nG2 X10 Y0 I5 F100\n"),
               ContainsSubstring("spindle"));
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
