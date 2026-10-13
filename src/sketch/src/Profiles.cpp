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

} // namespace lcad::sketch
