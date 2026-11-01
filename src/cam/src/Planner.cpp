// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/cam/Planner.h>

#include "Areas.h"

#include <lcad/kernel/Error.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/kernel/Transform.h>

#include <algorithm>
#include <charconv>
#include <cmath>
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

// A closed path the tool goes around, by the distance along it from where it starts. A distance
// can run on past one lap.
class Ring {
public:
    explicit Ring(Loop closed) : m_points(std::move(closed)) {
        m_distances.push_back(0.0);
        for (std::size_t i = 1; i < m_points.size(); ++i) {
            m_distances.push_back(m_distances.back() + apart(m_points[i - 1], m_points[i]));
        }
    }

    [[nodiscard]] double length() const { return m_distances.back(); }
    [[nodiscard]] const std::vector<double>& distances() const { return m_distances; }

    [[nodiscard]] Point2D at(double distance) const {
        const double along = std::fmod(distance, length());
        const auto after = std::upper_bound(m_distances.begin(), m_distances.end(), along);
        if (after == m_distances.begin() || after == m_distances.end()) {
            return m_points.front();
        }
        const auto next = static_cast<std::size_t>(after - m_distances.begin());
        const double t = (along - m_distances[next - 1]) / (*after - m_distances[next - 1]);
        return {m_points[next - 1].x + (m_points[next].x - m_points[next - 1].x) * t,
                m_points[next - 1].y + (m_points[next].y - m_points[next - 1].y) * t};
    }

private:
    Loop m_points;
    std::vector<double> m_distances;
};

// The bridges of material a contour leaves, along its ring.
struct Tabs {
    // Where the middle of each lies.
    std::vector<double> middles;
    // How far along the ring the tool stays up over each: the bridge, and the tool on either side.
    double span = 0.0;
    // How high the bridges come.
    double top = 0.0;
};

// How far along the ring the tool goes up the near side of a tab: all but straight up. It comes
// down the far side at 45 degrees, which keeps it from plunging into what it has yet to cut.
constexpr double tabRise = 0.01;

// How high the tool has to stay at a distance along a ring, cutting at `z`.
double heightOver(const Tabs& tabs, double along, double z, double length) {
    double height = z;
    if (z >= tabs.top) {
        return height;
    }
    const double fall = tabs.top - z;
    for (const double middle : tabs.middles) {
        const double past = std::fmod(along - (middle - tabs.span / 2.0) + 2.0 * length, length);
        if (past <= tabs.span) {
            height = tabs.top;
        } else if (past < tabs.span + fall) {
            height = std::max(height, tabs.top - (past - tabs.span));
        } else if (past > length - tabRise) {
            height = std::max(height, z + fall * (past - (length - tabRise)) / tabRise);
        }
    }
    return height;
}

// Around a ring from a distance along it, coming down at `slope` from `above` to `z`, and then
// once all the way around at that depth, which takes away what the ramp left. No slope goes
// straight down. Where the tool has to stay above tabs, it does. Ends where the lap ends, and says
// how far along that is.
double cutAround(Path& path, const Ring& ring, const Tabs& tabs, double from, double above,
                 double z, double slope) {
    const double length = ring.length();
    const double rampEnd = slope > 0.0 ? from + (above - z) / slope : from;
    const double end = rampEnd + length;
    const auto height = [&](double distance) {
        const double ramp = slope > 0.0 ? above - slope * (distance - from) : z;
        return std::max(ramp, heightOver(tabs, std::fmod(distance, length), z, length));
    };

    // Where it has to change direction or height, each of them once.
    std::vector<double> marks = ring.distances();
    if (z < tabs.top) {
        const double fall = tabs.top - z;
        for (const double middle : tabs.middles) {
            const double start = middle - tabs.span / 2.0;
            for (const double mark :
                 {start - tabRise, start, start + tabs.span, start + tabs.span + fall}) {
                marks.push_back(std::fmod(mark + length, length));
            }
        }
    }
    std::vector<double> stops{rampEnd, end};
    for (double lap = std::floor(from / length) * length; lap < end; lap += length) {
        for (const double mark : marks) {
            if (lap + mark > from && lap + mark < end) {
                stops.push_back(lap + mark);
            }
        }
    }
    std::sort(stops.begin(), stops.end());
    stops.erase(
        std::unique(stops.begin(), stops.end(), [](double a, double b) { return b - a < 1e-7; }),
        stops.end());

    const Point2D start = ring.at(from);
    path.plunge({start.x, start.y, height(from)});
    for (const double stop : stops) {
        if (stop <= from) {
            continue;
        }
        const Point2D place = ring.at(stop);
        const double high = height(stop);
        if (high < path.z() - 1e-9) {
            path.plunge({place.x, place.y, high});
        } else {
            path.feed({place.x, place.y, high});
        }
    }
    return end;
}

// How steeply to go down around a ring, as height for distance. One too short to ramp around is
// gone straight down into: the tool would only be turning on the spot.
double slopeAround(const Ring& ring, const Tool& tool, const MachiningOperation& operation) {
    if (operation.rampAngle <= 0.0 || ring.length() < tool.diameter / 2.0) {
        return 0.0;
    }
    return std::tan(operation.rampAngle * pi / 180.0);
}

// A circle to spiral down around, for where the path has no ring long enough: half as wide as the
// tool, so that it leaves nothing standing in the middle, or as much of that as there is room for.
std::optional<Ring> entryCircle(const Area& room, const Point2D& middle, double toolRadius) {
    constexpr int sides = 32;
    for (double radius = toolRadius / 2.0; radius >= toolRadius / 8.0; radius /= 2.0) {
        Loop circle;
        bool fits = true;
        for (int i = 0; i <= sides; ++i) {
            const double angle = 2.0 * pi * (i % sides) / sides;
            const Point2D point{middle.x + radius * std::cos(angle),
                                middle.y + radius * std::sin(angle)};
            fits = fits && pointInside(room, point);
            circle.push_back(point);
        }
        if (fits) {
            return Ring(std::move(circle));
        }
    }
    return std::nullopt;
}

// Places for the tabs around a ring: evenly, and each moved onto the nearest stretch that is
// straight for as long as it needs, where one is free. None if they do not all fit.
std::vector<double> tabPlaces(const Ring& ring, int count, double span, double spacing) {
    const double length = ring.length();
    const std::vector<double>& distances = ring.distances();
    const auto between = [length](double a, double b) {
        const double away = std::abs(a - b);
        return std::min(away, length - away);
    };
    std::vector<double> places;
    for (int i = 0; i < count; ++i) {
        const double wanted = length * (i + 0.5) / count;
        const auto roomFor = [&](double place) {
            return std::all_of(places.begin(), places.end(),
                               [&](double other) { return between(place, other) >= spacing; });
        };
        std::optional<double> best;
        for (std::size_t j = 1; j < distances.size(); ++j) {
            if (distances[j] - distances[j - 1] < span) {
                continue;
            }
            const double place =
                std::clamp(wanted, distances[j - 1] + span / 2.0, distances[j] - span / 2.0);
            if (roomFor(place) &&
                (!best.has_value() || between(place, wanted) < between(*best, wanted))) {
                best = place;
            }
        }
        if (!best.has_value()) {
            if (!roomFor(wanted)) {
                return {};
            }
            best = wanted;
        }
        places.push_back(*best);
    }
    return places;
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

bool checkRamp(const MachiningOperation& operation, std::vector<std::string>& problems) {
    if (operation.rampAngle < 0.0 || operation.rampAngle > 45.0) {
        problems.push_back("The ramp has to be between 0 degrees, straight down, and 45.");
        return false;
    }
    return true;
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
    if (!checkDepths(heights, tool, operation, problems) || !checkRamp(operation, problems)) {
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
            first = false;
            if (close && lineInside(room, path.at(), start)) {
                path.feed({start.x, start.y, z});
                for (std::size_t i = 1; i < ring.size(); ++i) {
                    path.feed({ring[i].x, ring[i].y, z});
                }
                continue;
            }
            const Ring around(ring);
            const bool wantsCircle = operation.rampAngle > 0.0 && around.length() < pi * radius;
            const std::optional<Ring> circle =
                wantsCircle ? entryCircle(room, loopMiddle(loop), radius) : std::nullopt;
            const Point2D entry = circle.has_value() ? circle->at(0.0) : start;
            path.travelTo(entry);
            if (above < heights.retract) {
                // What was cut before lets the tool down fast, nearly to the last depth.
                path.rapid({entry.x, entry.y, above + 0.5});
            }
            const double from = std::min(above, heights.top);
            if (circle.has_value()) {
                cutAround(path, *circle, {}, 0.0, from, z, slopeAround(*circle, tool, operation));
                path.feed({start.x, start.y, z});
                for (std::size_t i = 1; i < ring.size(); ++i) {
                    path.feed({ring[i].x, ring[i].y, z});
                }
            } else {
                cutAround(path, around, {}, 0.0, from, z, slopeAround(around, tool, operation));
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
    if (!checkDepths(heights, tool, operation, problems) || !checkRamp(operation, problems)) {
        return {};
    }
    if (operation.tabs < 0 ||
        (operation.tabs > 0 && (operation.tabWidth <= 0.0 || operation.tabHeight <= 0.0))) {
        problems.push_back("Tabs have to have a width and a height.");
        return {};
    }
    if (operation.tabs > 0 && operation.tabHeight >= heights.top - heights.bottom) {
        problems.push_back("The tabs are as high as the cut is deep: nothing would be cut there.");
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
        const Ring around(ring);
        Tabs tabs;
        if (operation.tabs > 0) {
            // The tool's middle stays a radius from a tab on either side of it.
            tabs.span = operation.tabWidth + tool.diameter;
            tabs.top = heights.bottom + operation.tabHeight;
            tabs.middles = tabPlaces(around, operation.tabs, tabs.span,
                                     tabs.span + operation.tabHeight + tabRise);
            if (tabs.middles.empty()) {
                problems.push_back(std::to_string(operation.tabs) +
                                   " tabs do not fit around this outline.");
                return {};
            }
        }
        const double slope = slopeAround(around, tool, operation);
        path.travelTo(ring.front());
        double along = 0.0;
        double above = heights.top;
        for (const double z : passDepths(heights.top, heights.bottom, operation.stepdown)) {
            along = cutAround(path, around, tabs, along, above, z, slope);
            above = z;
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
        // Below the stock is whatever it stands on. A hole that goes through needs that, and
        // what is under the work is the user's to know about.
        double lowest = work.stock.min.z;
        for (const Move& move : moves) {
            lowest = std::min(lowest, move.to.z);
        }
        if (lowest < work.stock.min.z - 1e-6) {
            say(operation.id,
                operation.name + " goes " + millimeters(work.stock.min.z - lowest) +
                    " below the stock: put something under the work that can take it.",
                Problem::Severity::Warning);
        }
        if (operation.kind == OperationKind::Contour && operation.tabs == 0 &&
            lowest <= work.stock.min.z + 1e-6) {
            say(operation.id,
                operation.name +
                    " cuts right through the stock, so what it cuts out comes loose: give it "
                    "tabs, or hold it down some other way.",
                Problem::Severity::Warning);
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
