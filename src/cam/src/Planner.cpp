// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/cam/Planner.h>

#include "Areas.h"

#include <lcad/kernel/Error.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/kernel/Transform.h>

#include <algorithm>
#include <cmath>
#include <charconv>
#include <limits>
#include <optional>

namespace lcad::cam {

using document::MachiningOperation;
using document::ObjectId;
using document::OperationKind;
using document::Setup;
using document::Tool;
using document::ToolKind;
using kernel::FlatRegion;
using kernel::Point;
using kernel::Point2D;

namespace {

constexpr double pi = 3.14159265358979323846;

double apart(const Point2D& a, const Point2D& b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

std::string millimeters(double value) {
    char text[64];
    const auto end =
        std::to_chars(text, text + sizeof(text), value, std::chars_format::fixed, 2).ptr;
    return std::string(text, end) + " mm";
}

// Collects the moves of a path, leaving out what goes nowhere.
class Path {
public:
    Path(const Heights& heights, const MachiningOperation& operation)
        : m_heights(heights), m_feed(operation.feed), m_plunge(operation.plungeFeed) {}

    [[nodiscard]] bool started() const { return !m_moves.empty(); }
    [[nodiscard]] Point2D at() const { return {m_at.x, m_at.y}; }
    [[nodiscard]] double z() const { return m_at.z; }

    void rapid(const Point& to) { add(MoveKind::Rapid, to, 0.0); }
    void feed(const Point& to) { add(MoveKind::Feed, to, m_feed); }
    void plunge(const Point& to) { add(MoveKind::Plunge, to, m_plunge); }

    // Up to the clearance height, over to above a place, and down to the retract height.
    void travelTo(const Point2D& place) {
        if (started()) {
            rapid({m_at.x, m_at.y, m_heights.clearance});
        }
        rapid({place.x, place.y, m_heights.clearance});
        rapid({place.x, place.y, m_heights.retract});
    }
    // Ends up at the clearance height.
    void finish() {
        if (started()) {
            rapid({m_at.x, m_at.y, m_heights.clearance});
        }
    }
    [[nodiscard]] std::vector<Move> take() { return std::move(m_moves); }

private:
    void add(MoveKind kind, const Point& to, double feed) {
        if (started() &&
            std::abs(to.x - m_at.x) + std::abs(to.y - m_at.y) + std::abs(to.z - m_at.z) < 1e-9) {
            return;
        }
        m_moves.push_back({kind, to, feed});
        m_at = to;
    }

    const Heights& m_heights;
    double m_feed;
    double m_plunge;
    std::vector<Move> m_moves;
    Point m_at;
};

// The loop started at its point nearest to `place`, and closed.
Loop closedFrom(const Loop& loop, const Point2D& place) {
    std::size_t first = 0;
    double nearest = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const double away = apart(loop[i], place);
        if (away < nearest) {
            nearest = away;
            first = i;
        }
    }
    Loop out;
    out.reserve(loop.size() + 1);
    for (std::size_t i = 0; i <= loop.size(); ++i) {
        out.push_back(loop[(first + i) % loop.size()]);
    }
    return out;
}

Point2D loopMiddle(const Loop& loop) {
    Point2D sum;
    for (const Point2D& p : loop) {
        sum.x += p.x;
        sum.y += p.y;
    }
    const auto n = static_cast<double>(loop.size());
    return {sum.x / n, sum.y / n};
}

bool checkDepths(const Heights& heights, const Tool& tool, const MachiningOperation& operation,
                 std::vector<std::string>& problems) {
    const bool good = heights.top - heights.bottom > 1e-9;
    if (!good) {
        problems.push_back("There is nothing to cut: it would go no deeper than where the "
                           "material starts.");
    }
    if (operation.stepdown <= 0.0) {
        problems.push_back("The stepdown has to be more than zero.");
        return false;
    }
    if (operation.stepdown > tool.fluteLength) {
        problems.push_back("The stepdown of " + millimeters(operation.stepdown) +
                           " is more than the " + tool.name + " can cut, " +
                           millimeters(tool.fluteLength) + ".");
        return false;
    }
    if (heights.top - heights.bottom > tool.stickout) {
        problems.push_back("It goes " + millimeters(heights.top - heights.bottom) +
                           " deep, further than the " + tool.name + " sticks out, " +
                           millimeters(tool.stickout) + ": the holder would hit the work.");
        return false;
    }
    return good;
}

} // namespace

std::vector<double> passDepths(double top, double bottom, double stepdown) {
    std::vector<double> depths;
    if (stepdown <= 0.0 || bottom >= top) {
        return depths;
    }
    const auto passes = static_cast<int>(std::ceil((top - bottom) / stepdown - 1e-9));
    for (int i = 1; i < passes; ++i) {
        depths.push_back(top - stepdown * i);
    }
    depths.push_back(bottom);
    return depths;
}

std::vector<Move> faceMoves(const kernel::BoundingBox& stock, const Heights& heights,
                            const Tool& tool, const MachiningOperation& operation,
                            std::vector<std::string>& problems) {
    if (tool.kind != ToolKind::FlatEndMill) {
        problems.push_back("Facing takes a flat end mill: " + tool.name +
                           " would leave a "
                           "surface that is not flat.");
        return {};
    }
    if (operation.stepover <= 0.0 || operation.stepover > tool.diameter * 0.95) {
        problems.push_back("The stepover has to be more than zero and less than the tool's "
                           "diameter, or ridges are left.");
        return {};
    }
    if (!checkDepths(heights, tool, operation, problems)) {
        return {};
    }
    const double radius = tool.diameter / 2.0;
    const double xFrom = stock.min.x - radius - 2.0;
    const double xTo = stock.max.x + radius + 2.0;
    Path path(heights, operation);
    for (const double z : passDepths(heights.top, heights.bottom, operation.stepdown)) {
        std::vector<double> rows;
        for (double y = stock.min.y; y < stock.max.y - 1e-6; y += operation.stepover) {
            rows.push_back(y);
        }
        rows.push_back(stock.max.y);
        // From outside the stock: the tool goes down where there is nothing to cut.
        path.travelTo({xFrom, rows.front()});
        path.plunge({xFrom, rows.front(), z});
        bool forward = true;
        for (std::size_t row = 0; row < rows.size(); ++row) {
            const double y = rows[row];
            path.feed({xFrom, y, z});
            path.feed({forward ? xTo : xFrom, y, z});
            forward = !forward;
            // The move to the next row is itself outside the stock.
            if (row + 1 < rows.size()) {
                path.feed({forward ? xFrom : xTo, rows[row + 1], z});
            }
        }
    }
    path.finish();
    return path.take();
}

std::vector<Move> pocketMoves(const FlatRegion& region, const Heights& heights, const Tool& tool,
                              const MachiningOperation& operation,
                              std::vector<std::string>& problems) {
    if (tool.kind == ToolKind::Drill) {
        problems.push_back("A drill cuts straight down only: a pocket takes an end mill.");
        return {};
    }
    if (operation.stepover <= 0.0 || operation.stepover > tool.diameter * 0.95) {
        problems.push_back("The stepover has to be more than zero and less than the tool's "
                           "diameter, or ridges are left.");
        return {};
    }
    if (!checkDepths(heights, tool, operation, problems)) {
        return {};
    }
    const double radius = tool.diameter / 2.0;
    const Area walls = areaOf(region);
    // Where the middle of the tool may go at all.
    const Area room = offset(walls, -(radius + operation.stockToLeave));
    if (room.empty()) {
        problems.push_back("The " + tool.name + " is too large for this pocket.");
        return {};
    }
    // Rings from the walls inward, as far as there is room.
    std::vector<Area> rings;
    for (Area ring = room; !ring.empty(); ring = offset(ring, -operation.stepover)) {
        rings.push_back(ring);
        if (rings.size() > 10000) {
            problems.push_back("The stepover is too small for a pocket this large.");
            return {};
        }
    }

    // Each loop of a ring once, from the middle outward.
    std::vector<Loop> loops;
    for (auto ring = rings.rbegin(); ring != rings.rend(); ++ring) {
        for (const Loop& loop : *ring) {
            loops.push_back(loop);
        }
    }

    // What the tool's path reaches has to be all that it can reach. What a round tool cannot
    // reach, such as a sharp inside corner, is not a failing of the path.
    const Area reachable = offset(room, radius);
    const Area reached = swept(loops, true, radius);
    const double missed = sizeOf(difference(reachable, reached));
    const double tolerated = std::max(0.01 * sizeOf(reachable), 0.01);
    if (missed > tolerated) {
        problems.push_back("The passes would leave material standing: try a smaller stepover.");
        return {};
    }

    Path path(heights, operation);
    double above = heights.retract;
    for (const double z : passDepths(heights.top, heights.bottom, operation.stepdown)) {
        bool first = true;
        for (const Loop& loop : loops) {
            const Loop ring = closedFrom(loop, path.started() ? path.at() : loopMiddle(loop));
            const Point2D start = ring.front();
            // Across to the next ring at depth if that stays in the pocket and is near, so that
            // it cuts only what is left between them; otherwise over the top.
            const bool close = !first && apart(path.at(), start) <= operation.stepover * 1.5 + 1e-6;
            if (close && lineInside(room, path.at(), start)) {
                path.feed({start.x, start.y, z});
            } else {
                path.travelTo(start);
                if (above < heights.retract) {
                    // What was cut before lets the tool down fast, nearly to the last depth.
                    path.rapid({start.x, start.y, above + 0.5});
                }
                path.plunge({start.x, start.y, z});
            }
            first = false;
            for (std::size_t i = 1; i < ring.size(); ++i) {
                path.feed({ring[i].x, ring[i].y, z});
            }
        }
        above = z;
    }
    path.finish();
    return path.take();
}

std::vector<Move> contourMoves(const FlatRegion& region, const Heights& heights, const Tool& tool,
                               const MachiningOperation& operation,
                               std::vector<std::string>& problems) {
    if (tool.kind == ToolKind::Drill) {
        problems.push_back("A drill cuts straight down only: a contour takes an end mill.");
        return {};
    }
    if (!checkDepths(heights, tool, operation, problems)) {
        return {};
    }
    const double radius = tool.diameter / 2.0;
    const bool outside = operation.side == document::ContourSide::Outside;
    const double reach = radius + operation.stockToLeave;
    FlatRegion outline = region;
    outline.holes.clear();
    const Area paths = offset(areaOf(outline), outside ? reach : -reach);
    if (paths.empty()) {
        problems.push_back("The " + tool.name +
                           " is too large to go around the inside of this "
                           "outline.");
        return {};
    }
    Path path(heights, operation);
    for (const Loop& loop : paths) {
        Loop ring = closedFrom(loop, path.started() ? path.at() : loopMiddle(loop));
        // With the material on the right of the way the tool goes: a climb cut, for a tool that
        // turns clockwise. An offset's outlines run counter-clockwise.
        if (outside) {
            std::reverse(ring.begin(), ring.end());
        }
        const Point2D start = ring.front();
        path.travelTo(start);
        for (const double z : passDepths(heights.top, heights.bottom, operation.stepdown)) {
            path.plunge({start.x, start.y, z});
            for (std::size_t i = 1; i < ring.size(); ++i) {
                path.feed({ring[i].x, ring[i].y, z});
            }
        }
    }
    path.finish();
    return path.take();
}

std::vector<Move> drillMoves(const std::vector<kernel::RoundFace>& holes, double partBottom,
                             const Heights& heights, const Tool& tool,
                             const MachiningOperation& operation,
                             std::vector<std::string>& problems) {
    if (holes.empty()) {
        problems.push_back("There are no holes to drill: choose their round faces.");
        return {};
    }
    if (tool.kind == ToolKind::BallEndMill) {
        problems.push_back("A ball end mill does not drill: take a drill or a flat end mill.");
        return {};
    }
    for (const kernel::RoundFace& hole : holes) {
        if (std::abs(hole.radius * 2.0 - tool.diameter) > 0.05) {
            problems.push_back("The " + tool.name + " makes a hole of " +
                               millimeters(tool.diameter) + ", and this hole is " +
                               millimeters(hole.radius * 2.0) + ".");
            return {};
        }
    }
    // Nearest next, from the first.
    std::vector<kernel::RoundFace> left = holes;
    std::vector<kernel::RoundFace> ordered;
    Point2D at = left.front().center;
    while (!left.empty()) {
        const auto next =
            std::min_element(left.begin(), left.end(), [&at](const auto& a, const auto& b) {
                return apart(a.center, at) < apart(b.center, at);
            });
        at = next->center;
        ordered.push_back(*next);
        left.erase(next);
    }

    // A drill's point goes deeper than where its full diameter ends. A hole that goes through
    // takes that, and a little more; one that stops has its point at the bottom.
    const double tip = tool.kind == ToolKind::Drill
                           ? tool.diameter / 2.0 / std::tan(tool.tipAngle * pi / 360.0)
                           : 0.0;
    Path path(heights, operation);
    for (const kernel::RoundFace& hole : ordered) {
        const bool through = hole.bottom <= partBottom + 1e-6;
        const double bottom = through ? hole.bottom - tip - 0.5 : hole.bottom;
        const double deepest = heights.top - bottom;
        if (deepest > tool.fluteLength) {
            problems.push_back("The hole goes " + millimeters(deepest) + " deep, more than the " +
                               tool.name + " can drill, " + millimeters(tool.fluteLength) + ".");
            return {};
        }
        path.travelTo(hole.center);
        const double top = heights.top;
        if (operation.peck > 0.0) {
            // In and out again to clear the chips, down to a little above the last peck.
            double reached = top;
            for (const double z : passDepths(top, bottom, operation.peck)) {
                path.plunge({hole.center.x, hole.center.y, z});
                reached = z;
                if (z > bottom) {
                    path.rapid({hole.center.x, hole.center.y, heights.retract});
                    path.rapid({hole.center.x, hole.center.y, reached + 0.5});
                }
            }
        } else {
            path.plunge({hole.center.x, hole.center.y, bottom});
        }
        path.rapid({hole.center.x, hole.center.y, heights.retract});
    }
    path.finish();
    return path.take();
}

namespace {

// A body of the document where it is now, by the names an operation holds.
struct Target {
    kernel::Shape face;
    double lowest = 0.0;
};

std::optional<kernel::Shape> faceOf(const document::Document& document,
                                    const document::OperationTarget& target) {
    const kernel::NamedShape named = document.namedBody(target.body);
    if (named.shape.isNull()) {
        return std::nullopt;
    }
    const int index = kernel::findFace(named, target.face);
    if (index < 0) {
        return std::nullopt;
    }
    kernel::Placement placement;
    for (const document::Instance& instance : document.instances()) {
        const document::Body* body = document.findBody(target.body);
        if (instance.path == target.path && body != nullptr &&
            body->component == instance.component) {
            placement = instance.placement;
        }
    }
    return kernel::placed(kernel::subShape(named.shape, kernel::SubShapeKind::Face, index),
                          placement);
}

} // namespace

WorkArea workAreaOf(const document::Document& document, const Setup& setup) {
    std::vector<kernel::Shape> shapes;
    for (const document::Instance& instance : document.instances()) {
        if (!instance.visible) {
            continue;
        }
        for (const document::Body& body : document.bodies()) {
            if (body.component == instance.component && !body.shape.isNull() && body.visible) {
                shapes.push_back(kernel::placed(body.shape, instance.placement));
            }
        }
    }
    WorkArea work;
    const auto bounds = kernel::boundingBox(kernel::makeCompound(shapes));
    if (!bounds.has_value()) {
        return work;
    }
    work.part = *bounds;
    work.stock = *bounds;
    work.stock.min.x -= setup.stock.side;
    work.stock.min.y -= setup.stock.side;
    work.stock.max.x += setup.stock.side;
    work.stock.max.y += setup.stock.side;
    work.stock.max.z += setup.stock.top;
    work.stock.min.z -= setup.stock.bottom;
    if (setup.origin == document::WorkOrigin::StockTopCorner) {
        work.origin = {work.stock.min.x, work.stock.min.y, work.stock.max.z};
    } else {
        work.origin = {(work.stock.min.x + work.stock.max.x) / 2.0,
                       (work.stock.min.y + work.stock.max.y) / 2.0, work.stock.max.z};
    }
    return work;
}

Program planSetup(const document::Document& document, const Setup& setup) {
    Program program;
    program.work = workAreaOf(document, setup);
    const WorkArea& work = program.work;
    const auto say = [&program](ObjectId operation, std::string text,
                                Problem::Severity severity = Problem::Severity::Error) {
        program.problems.push_back({severity, operation, std::move(text)});
    };
    const double stockHigh = work.stock.max.z - work.stock.min.z;
    if (stockHigh <= 0.0) {
        say({}, "There is no body to machine.");
        return program;
    }
    program.safeZ = work.stock.max.z + 5.0;

    for (const MachiningOperation& operation : setup.operations) {
        if (operation.suppressed) {
            continue;
        }
        const Tool* tool = document.findTool(operation.tool);
        if (tool == nullptr) {
            say(operation.id, operation.name + " has no tool.");
            continue;
        }
        if (operation.feed <= 0.0 || operation.plungeFeed <= 0.0) {
            say(operation.id, operation.name + " needs a feed above zero.");
            continue;
        }
        if (operation.spindle <= 0.0) {
            say(operation.id, operation.name + " needs the spindle to turn.");
            continue;
        }
        if (operation.retract <= 0.0 || operation.clearance < operation.retract) {
            say(operation.id,
                operation.name +
                    " needs a clearance above the stock, and a "
                    "retract height above the stock and no higher than the clearance.");
            continue;
        }
        if (operation.feed > setup.machine.maxFeed ||
            operation.plungeFeed > setup.machine.maxFeed) {
            say(operation.id, operation.name + " feeds faster than the machine can.");
            continue;
        }
        if (operation.spindle > setup.machine.maxSpindle) {
            say(operation.id, operation.name + " turns the spindle faster than the machine can.");
            continue;
        }

        Heights heights;
        heights.top = work.stock.max.z;
        heights.clearance = work.stock.max.z + operation.clearance;
        heights.retract = work.stock.max.z + operation.retract;
        program.safeZ = std::max(program.safeZ, heights.clearance);

        std::vector<Move> moves;
        std::vector<std::string> trouble;
        try {
            switch (operation.kind) {
            case OperationKind::Face:
                heights.bottom = work.part.max.z + operation.stockToLeave;
                moves = faceMoves(work.stock, heights, *tool, operation, trouble);
                break;
            case OperationKind::Pocket:
            case OperationKind::Contour: {
                if (operation.targets.empty()) {
                    trouble.push_back("Choose a flat face for it to work on.");
                    break;
                }
                for (const document::OperationTarget& target : operation.targets) {
                    const auto face = faceOf(document, target);
                    const auto plane = face.has_value() ? kernel::planeOfFace(*face) : std::nullopt;
                    if (!plane.has_value()) {
                        trouble.push_back("A face it works on is not in the model any more.");
                        continue;
                    }
                    if (std::abs(std::abs(plane->normal.z) - 1.0) > 1e-9) {
                        trouble.push_back("The face has to be level: the tool comes straight "
                                          "down, along Z.");
                        continue;
                    }
                    const FlatRegion region =
                        kernel::flatOutline(*face, kernel::levelPlane(plane->origin.z));
                    heights.bottom = plane->origin.z + operation.stockToLeave;
                    const std::vector<Move> part =
                        operation.kind == OperationKind::Pocket
                            ? pocketMoves(region, heights, *tool, operation, trouble)
                            : contourMoves(region, heights, *tool, operation, trouble);
                    moves.insert(moves.end(), part.begin(), part.end());
                }
                break;
            }
            case OperationKind::Drill: {
                std::vector<kernel::RoundFace> holes;
                for (const document::OperationTarget& target : operation.targets) {
                    const auto face = faceOf(document, target);
                    const auto round =
                        face.has_value() ? kernel::roundFaceAlongZ(*face) : std::nullopt;
                    if (!round.has_value()) {
                        trouble.push_back("A hole it works on is not a round face along Z.");
                        continue;
                    }
                    holes.push_back(*round);
                }
                moves = drillMoves(holes, work.part.min.z, heights, *tool, operation, trouble);
                break;
            }
            }
        } catch (const kernel::Error& error) {
            trouble.push_back(error.reason());
        }
        for (std::string& text : trouble) {
            say(operation.id, operation.name + ": " + text);
        }
        if (!trouble.empty() || moves.empty()) {
            continue;
        }
        Toolpath toolpath;
        toolpath.operation = operation.id;
        toolpath.name = operation.name;
        toolpath.tool = *tool;
        toolpath.spindle = operation.spindle;
        toolpath.moves = std::move(moves);
        program.paths.push_back(std::move(toolpath));
    }

    // Everything has to lie within what the machine can reach, from wherever the work is put.
    bool any = false;
    Point low;
    Point high;
    const auto take = [&](const Point& p) {
        if (!any) {
            low = high = p;
            any = true;
            return;
        }
        low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    };
    for (const Toolpath& path : program.paths) {
        for (const Move& move : path.moves) {
            take(move.to);
        }
    }
    if (any) {
        const auto check = [&](double span, double travel, const char* axis) {
            if (span > travel) {
                say({}, std::string("The toolpaths reach ") + millimeters(span) + " along " + axis +
                            ", and the machine travels " + millimeters(travel) + ".");
            }
        };
        check(high.x - low.x, setup.machine.travelX, "X");
        check(high.y - low.y, setup.machine.travelY, "Y");
        check(high.z - low.z, setup.machine.travelZ, "Z");
    }
    return program;
}

} // namespace lcad::cam
