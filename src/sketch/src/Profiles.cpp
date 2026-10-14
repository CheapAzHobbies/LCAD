// SPDX-License-Identifier: GPL-3.0-or-later

// Finds the closed outlines of a sketch.
//
// The ordinary lines and arcs are the edges of a graph whose nodes are the places where their
// ends meet. Walking that graph, always taking the next edge clockwise at each node, goes around
// one region at a time, which is how the regions of a map are found. Each region that is walked
// counter-clockwise is an outline; the one walked clockwise is the outside of the whole figure.
//
// Curves are taken to meet only at their ends. Two curves that cross somewhere in the middle are
// not split there.

#include <lcad/sketch/Sketch.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace lcad::sketch {

namespace {

constexpr double pi = 3.14159265358979323846;
// Ends closer than this, in millimeters, are joined.
constexpr double joinTolerance = 1e-6;

struct Edge {
    EntityId curve = 0;
    bool isArc = false;
    int from = 0; // node at the curve's start
    int to = 0;   // node at its end
    Point2 start;
    Point2 end;
    Point2 center; // arcs
    bool alive = true;
};

// One direction along an edge: 2 * edge for start to end, 2 * edge + 1 for the way back.
struct Outgoing {
    int half = 0;
    double angle = 0.0;
};

double counterClockwiseSweep(const Edge& arc) {
    const double a0 = std::atan2(arc.start.y - arc.center.y, arc.start.x - arc.center.x);
    const double a1 = std::atan2(arc.end.y - arc.center.y, arc.end.x - arc.center.x);
    double sweep = a1 - a0;
    while (sweep <= 0.0) {
        sweep += 2.0 * pi;
    }
    return sweep;
}

// The direction in which a walk leaves a node along one direction of an edge.
double leavingAngle(const Edge& edge, bool forward) {
    if (!edge.isArc) {
        const Point2 a = forward ? edge.start : edge.end;
        const Point2 b = forward ? edge.end : edge.start;
        return std::atan2(b.y - a.y, b.x - a.x);
    }
    if (forward) {
        // Counter-clockwise around the center, starting at the start.
        return std::atan2(edge.start.x - edge.center.x, -(edge.start.y - edge.center.y));
    }
    return std::atan2(-(edge.end.x - edge.center.x), edge.end.y - edge.center.y);
}

// What one direction along an edge adds to the area enclosed by a walk.
double areaTerm(const Edge& edge, bool forward) {
    const Point2 a = forward ? edge.start : edge.end;
    const Point2 b = forward ? edge.end : edge.start;
    double term = (a.x * b.y - b.x * a.y) / 2.0;
    if (edge.isArc) {
        const double radius =
            std::hypot(edge.start.x - edge.center.x, edge.start.y - edge.center.y);
        const double sweep = counterClockwiseSweep(edge);
        // The sliver between the arc and its chord.
        const double sliver = radius * radius / 2.0 * (sweep - std::sin(sweep));
        term += forward ? sliver : -sliver;
    }
    return term;
}

} // namespace

std::vector<Profile> Sketch::profiles() const {
    std::vector<Profile> found;
    std::vector<Edge> edges;
    std::vector<Point2> nodes;
    const auto nodeAt = [&nodes](Point2 at) {
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (std::hypot(nodes[i].x - at.x, nodes[i].y - at.y) <= joinTolerance) {
                return static_cast<int>(i);
            }
        }
        nodes.push_back(at);
        return static_cast<int>(nodes.size()) - 1;
    };

    for (const Entity& item : m_entities) {
        if (item.construction || item.kind == EntityKind::Point) {
            continue;
        }
        if (item.kind == EntityKind::Circle) {
            found.push_back({{item.id}, pi * item.radius * item.radius});
            continue;
        }
        Edge edge;
        edge.curve = item.id;
        edge.isArc = item.kind == EntityKind::Arc;
        edge.start = position(item.start);
        edge.end = position(item.end);
        if (edge.isArc) {
            edge.center = position(item.center);
        }
        edge.from = nodeAt(edge.start);
        edge.to = nodeAt(edge.end);
        if (edge.from != edge.to) {
            edges.push_back(edge);
        }
    }

    // A curve with a loose end cannot be part of an outline, and removing it may loosen another.
    for (bool changed = true; changed;) {
        changed = false;
        std::vector<int> degree(nodes.size(), 0);
        for (const Edge& edge : edges) {
            if (edge.alive) {
                ++degree[static_cast<std::size_t>(edge.from)];
                ++degree[static_cast<std::size_t>(edge.to)];
            }
        }
        for (Edge& edge : edges) {
            if (edge.alive && (degree[static_cast<std::size_t>(edge.from)] < 2 ||
                               degree[static_cast<std::size_t>(edge.to)] < 2)) {
                edge.alive = false;
                changed = true;
            }
        }
    }

    // Around each node, the directions that leave it, counter-clockwise.
    std::vector<std::vector<Outgoing>> around(nodes.size());
    for (std::size_t i = 0; i < edges.size(); ++i) {
        const Edge& edge = edges[i];
        if (!edge.alive) {
            continue;
        }
        const int half = static_cast<int>(2 * i);
        around[static_cast<std::size_t>(edge.from)].push_back({half, leavingAngle(edge, true)});
        around[static_cast<std::size_t>(edge.to)].push_back({half + 1, leavingAngle(edge, false)});
    }
    for (std::vector<Outgoing>& leaving : around) {
        std::sort(leaving.begin(), leaving.end(),
                  [](const Outgoing& a, const Outgoing& b) { return a.angle < b.angle; });
    }

    std::vector<bool> walked(2 * edges.size(), false);
    for (std::size_t first = 0; first < 2 * edges.size(); ++first) {
        if (walked[first] || !edges[first / 2].alive) {
            continue;
        }
        Profile outline;
        double area = 0.0;
        std::size_t half = first;
        bool closed = false;
        for (std::size_t steps = 0; steps <= 2 * edges.size(); ++steps) {
            walked[half] = true;
            const Edge& edge = edges[half / 2];
            const bool forward = half % 2 == 0;
            outline.curves.push_back(edge.curve);
            area += areaTerm(edge, forward);
            // At the node this arrives at, turn to the next direction clockwise from the way
            // back.
            const std::vector<Outgoing>& leaving =
                around[static_cast<std::size_t>(forward ? edge.to : edge.from)];
            const int back = static_cast<int>(half ^ 1U);
            std::size_t place = 0;
            while (place < leaving.size() && leaving[place].half != back) {
                ++place;
            }
            if (place == leaving.size()) {
                break;
            }
            half = static_cast<std::size_t>(
                leaving[(place + leaving.size() - 1) % leaving.size()].half);
            if (half == first) {
                closed = true;
                break;
            }
        }
        // Counter-clockwise walks enclose a region. The clockwise one is the outside.
        if (closed && area > joinTolerance) {
            outline.area = area;
            found.push_back(std::move(outline));
        }
    }

    std::sort(found.begin(), found.end(), [](const Profile& a, const Profile& b) {
        return *std::min_element(a.curves.begin(), a.curves.end()) <
               *std::min_element(b.curves.begin(), b.curves.end());
    });
    return found;
}

std::vector<Point2> Sketch::outlinePolygon(const std::vector<EntityId>& curves) const {
    // Each curve as points from its start to its end.
    const auto pointsOf = [this](const Entity& item) {
        std::vector<Point2> points;
        if (item.kind == EntityKind::Line) {
            points = {position(item.start), position(item.end)};
            return points;
        }
        const Point2 center = position(item.center);
        const double radius = radiusOf(item.id);
        double from = 0.0;
        double sweep = 2.0 * pi;
        if (item.kind == EntityKind::Arc) {
            const Point2 start = position(item.start);
            const Point2 end = position(item.end);
            from = std::atan2(start.y - center.y, start.x - center.x);
            sweep = std::atan2(end.y - center.y, end.x - center.x) - from;
            while (sweep <= 0.0) {
                sweep += 2.0 * pi;
            }
        }
        // A chord every three degrees.
        const int pieces = std::max(2, static_cast<int>(std::ceil(sweep / (pi / 60.0))));
        for (int i = 0; i <= pieces; ++i) {
            const double angle = from + sweep * i / pieces;
            points.push_back(
                {center.x + radius * std::cos(angle), center.y + radius * std::sin(angle)});
        }
        return points;
    };
    const auto apart = [](Point2 a, Point2 b) { return std::hypot(b.x - a.x, b.y - a.y); };

    std::vector<std::vector<Point2>> pieces;
    for (const EntityId id : curves) {
        const Entity* item = find(id);
        if (item != nullptr && item->kind != EntityKind::Point) {
            pieces.push_back(pointsOf(*item));
        }
    }
    std::vector<Point2> polygon;
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        std::vector<Point2>& piece = pieces[i];
        bool reversed = false;
        if (polygon.empty() && pieces.size() > 1) {
            // Start the first curve at the end the second does not share.
            const std::vector<Point2>& next = pieces[1];
            const auto meets = [&](Point2 p) {
                return apart(p, next.front()) <= joinTolerance ||
                       apart(p, next.back()) <= joinTolerance;
            };
            reversed = meets(piece.front()) && !meets(piece.back());
        } else if (!polygon.empty()) {
            reversed = apart(polygon.back(), piece.back()) < apart(polygon.back(), piece.front());
        }
        if (reversed) {
            std::reverse(piece.begin(), piece.end());
        }
        // The first point of a piece is the last point of the one before.
        polygon.insert(polygon.end(), piece.begin() + (polygon.empty() ? 0 : 1), piece.end());
    }
    if (polygon.size() > 1 && apart(polygon.front(), polygon.back()) <= joinTolerance) {
        polygon.pop_back();
    }
    return polygon;
}

namespace {

// How a point lies to a polygon: inside, outside, or on its edge to within the tolerance.
enum class Side { Inside, Outside, OnEdge };

Side sideOf(const std::vector<Point2>& polygon, Point2 p) {
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const Point2 a = polygon[j];
        const Point2 b = polygon[i];
        // The distance from the point to this side.
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        const double lengthSquared = dx * dx + dy * dy;
        const double along =
            lengthSquared > 0.0
                ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / lengthSquared, 0.0, 1.0)
                : 0.0;
        if (std::hypot(p.x - (a.x + along * dx), p.y - (a.y + along * dy)) <= 1e-5) {
            return Side::OnEdge;
        }
        if ((a.y > p.y) != (b.y > p.y) && p.x < a.x + (p.y - a.y) / (b.y - a.y) * dx) {
            inside = !inside;
        }
    }
    return inside ? Side::Inside : Side::Outside;
}

} // namespace

std::vector<Region> Sketch::regions() const {
    const std::vector<Profile> outlines = profiles();
    std::vector<std::vector<Point2>> polygons;
    polygons.reserve(outlines.size());
    for (const Profile& outline : outlines) {
        polygons.push_back(outlinePolygon(outline.curves));
    }
    // Whether outline `inner` lies within outline `outer`: smaller, with no point outside it
    // and at least one point properly inside. Outlines that only share a side are neighbors.
    const auto within = [&](std::size_t inner, std::size_t outer) {
        if (inner == outer || outlines[inner].area >= outlines[outer].area) {
            return false;
        }
        bool properlyInside = false;
        for (const Point2 point : polygons[inner]) {
            const Side side = sideOf(polygons[outer], point);
            if (side == Side::Outside) {
                return false;
            }
            properlyInside = properlyInside || side == Side::Inside;
        }
        return properlyInside;
    };

    std::vector<Region> found;
    for (std::size_t i = 0; i < outlines.size(); ++i) {
        Region region;
        region.outer = outlines[i].curves;
        region.area = outlines[i].area;
        for (std::size_t j = 0; j < outlines.size(); ++j) {
            if (!within(j, i)) {
                continue;
            }
            // A hole of this region, unless it lies within another outline that does.
            bool direct = true;
            for (std::size_t k = 0; direct && k < outlines.size(); ++k) {
                direct = !(k != i && k != j && within(j, k) && within(k, i));
            }
            if (direct) {
                region.holes.push_back(outlines[j].curves);
                region.area -= outlines[j].area;
            }
        }
        found.push_back(std::move(region));
    }
    return found;
}

} // namespace lcad::sketch
