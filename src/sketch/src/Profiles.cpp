// SPDX-License-Identifier: GPL-3.0-or-later

// Finds the closed outlines of a sketch.
//
// The ordinary lines and arcs are the edges of a graph whose nodes are the places where their
// ends meet. Walking that graph, always taking the next edge clockwise at each node, goes around
// one region at a time, which is how the regions of a map are found. Each region that is walked
// counter-clockwise is an outline; the one walked clockwise is the outside of the whole figure.
//
// For profiles(), curves are taken to meet only at their ends: two that cross somewhere in the
// middle are not split there. regions() does split the outlines that cross, at the crossings, and
// walks the pieces the same way, for the areas they cut each other into.

#include <lcad/sketch/Sketch.h>

#include <lcad/sketch/Edit.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

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
    // Splines: points along it from its start to its end, close together.
    std::vector<Point2> path;
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
    if (!edge.path.empty()) {
        const Point2 a = forward ? edge.path[0] : edge.path[edge.path.size() - 1];
        const Point2 b = forward ? edge.path[1] : edge.path[edge.path.size() - 2];
        return std::atan2(b.y - a.y, b.x - a.x);
    }
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

// One way round a region: directions along edges (see Outgoing), and the area it encloses.
struct Walk {
    std::vector<std::size_t> halves;
    double area = 0.0;
};

double areaTerm(const Edge& edge, bool forward);

// The walks around the regions that the alive edges divide the plane into.
std::vector<Walk> walksOf(const std::vector<Edge>& edges, std::size_t nodes) {
    // Around each node, the directions that leave it, counter-clockwise.
    std::vector<std::vector<Outgoing>> around(nodes);
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

    std::vector<Walk> found;
    std::vector<bool> walked(2 * edges.size(), false);
    for (std::size_t first = 0; first < 2 * edges.size(); ++first) {
        if (walked[first] || !edges[first / 2].alive) {
            continue;
        }
        Walk walk;
        std::size_t half = first;
        bool closed = false;
        for (std::size_t steps = 0; steps <= 2 * edges.size(); ++steps) {
            walked[half] = true;
            const Edge& edge = edges[half / 2];
            const bool forward = half % 2 == 0;
            walk.halves.push_back(half);
            walk.area += areaTerm(edge, forward);
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
        if (closed && walk.area > joinTolerance) {
            found.push_back(std::move(walk));
        }
    }
    return found;
}

// What one direction along an edge adds to the area enclosed by a walk.
double areaTerm(const Edge& edge, bool forward) {
    if (!edge.path.empty()) {
        double term = 0.0;
        for (std::size_t i = 1; i < edge.path.size(); ++i) {
            const Point2 a = edge.path[i - 1];
            const Point2 b = edge.path[i];
            term += (a.x * b.y - b.x * a.y) / 2.0;
        }
        return forward ? term : -term;
    }
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
        if (item.kind == EntityKind::Ellipse) {
            const Point2 c = position(item.center);
            const Point2 m = position(item.start);
            found.push_back({{item.id}, pi * std::hypot(m.x - c.x, m.y - c.y) * item.radius});
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
        if (item.kind == EntityKind::Spline) {
            edge.path = samples(item.id, 120);
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

    for (const Walk& walk : walksOf(edges, nodes.size())) {
        Profile outline;
        outline.area = walk.area;
        for (const std::size_t half : walk.halves) {
            outline.curves.push_back(edges[half / 2].curve);
        }
        found.push_back(std::move(outline));
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
        if (item.kind == EntityKind::Ellipse || item.kind == EntityKind::Spline) {
            return samples(item.id, 120);
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

std::vector<Region> Sketch::outlineRegions() const {
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
        region.key = region.outer;
        std::sort(region.key.begin(), region.key.end());
        found.push_back(std::move(region));
    }
    return found;
}

namespace {

// The area of a walk as points around it, with arcs cut into short chords.
std::vector<Point2> polygonOf(const Walk& walk, const std::vector<Edge>& edges) {
    std::vector<Point2> polygon;
    for (const std::size_t half : walk.halves) {
        const Edge& edge = edges[half / 2];
        const bool forward = half % 2 == 0;
        if (!edge.isArc) {
            polygon.push_back(forward ? edge.start : edge.end);
            continue;
        }
        const double radius =
            std::hypot(edge.start.x - edge.center.x, edge.start.y - edge.center.y);
        const double from = std::atan2(edge.start.y - edge.center.y, edge.start.x - edge.center.x);
        const double sweep = counterClockwiseSweep(edge);
        const int chords = std::max(2, static_cast<int>(std::ceil(sweep / (pi / 60.0))));
        for (int i = 0; i < chords; ++i) {
            const double along = forward ? double(i) / chords : 1.0 - double(i) / chords;
            const double angle = from + sweep * along;
            polygon.push_back({edge.center.x + radius * std::cos(angle),
                               edge.center.y + radius * std::sin(angle)});
        }
    }
    return polygon;
}

} // namespace

std::vector<Region> Sketch::regions() const {
    std::vector<Region> whole = outlineRegions();
    const std::vector<Profile> outlines = profiles();
    const std::size_t count = outlines.size();
    std::vector<std::vector<Point2>> polygons;
    polygons.reserve(count);
    for (const Profile& outline : outlines) {
        polygons.push_back(outlinePolygon(outline.curves));
    }
    const auto simple = [this](EntityId curve) {
        const Entity* item = find(curve);
        return item != nullptr &&
               (item->kind == EntityKind::Line || item->kind == EntityKind::Arc ||
                item->kind == EntityKind::Circle);
    };
    // Whether a place is an end of a curve rather than somewhere along it.
    const auto atEnd = [this](EntityId curve, Point2 p) {
        const Entity* item = find(curve);
        if (item->kind == EntityKind::Circle) {
            return false;
        }
        const Point2 a = position(item->start);
        const Point2 b = position(item->end);
        return std::hypot(a.x - p.x, a.y - p.y) <= joinTolerance * 10.0 ||
               std::hypot(b.x - p.x, b.y - p.y) <= joinTolerance * 10.0;
    };
    const auto crossing = [&](std::size_t i, std::size_t j) {
        for (const EntityId a : outlines[i].curves) {
            for (const EntityId b : outlines[j].curves) {
                if (a == b) {
                    // Neighbors, with a curve between them.
                    return false;
                }
            }
        }
        for (const EntityId a : outlines[i].curves) {
            for (const EntityId b : outlines[j].curves) {
                if (!simple(a) || !simple(b)) {
                    continue;
                }
                for (const Point2 p : intersections(*this, a, b)) {
                    if (!atEnd(a, p) && !atEnd(b, p)) {
                        return true;
                    }
                }
            }
        }
        return false;
    };
    std::vector<std::size_t> parent(count);
    std::iota(parent.begin(), parent.end(), std::size_t{0});
    const auto root = [&parent](std::size_t i) {
        while (parent[i] != i) {
            i = parent[i] = parent[parent[i]];
        }
        return i;
    };
    bool any = false;
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = i + 1; j < count; ++j) {
            if (crossing(i, j)) {
                parent[root(i)] = root(j);
                any = true;
            }
        }
    }
    if (!any) {
        return whole;
    }

    std::vector<Region> found;
    for (std::size_t first = 0; first < count; ++first) {
        if (root(first) != first) {
            continue;
        }
        std::vector<std::size_t> group;
        for (std::size_t i = 0; i < count; ++i) {
            if (root(i) == first) {
                group.push_back(i);
            }
        }
        std::vector<EntityId> curves;
        for (const std::size_t i : group) {
            curves.insert(curves.end(), outlines[i].curves.begin(), outlines[i].curves.end());
        }
        std::sort(curves.begin(), curves.end());
        curves.erase(std::unique(curves.begin(), curves.end()), curves.end());
        // Crossings can only be worked out for lines, circles, and arcs.
        if (group.size() < 2 || !std::all_of(curves.begin(), curves.end(), simple)) {
            for (const std::size_t i : group) {
                found.push_back(whole[i]);
            }
            continue;
        }

        // The pieces the curves are cut into, as the edges of a graph.
        std::vector<Edge> edges;
        std::vector<Point2> nodes;
        const auto nodeAt = [&nodes](Point2 at) {
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                if (std::hypot(nodes[i].x - at.x, nodes[i].y - at.y) <= joinTolerance * 10.0) {
                    return static_cast<int>(i);
                }
            }
            nodes.push_back(at);
            return static_cast<int>(nodes.size()) - 1;
        };
        for (const EntityId curve : curves) {
            const Entity& item = *find(curve);
            std::vector<Point2> cuts;
            for (const EntityId other : curves) {
                if (other != curve) {
                    const std::vector<Point2> here = intersections(*this, curve, other);
                    cuts.insert(cuts.end(), here.begin(), here.end());
                }
            }
            if (item.kind == EntityKind::Line) {
                const Point2 a = position(item.start);
                const Point2 b = position(item.end);
                cuts.push_back(a);
                cuts.push_back(b);
                const auto along = [a](Point2 p) { return std::hypot(p.x - a.x, p.y - a.y); };
                std::sort(cuts.begin(), cuts.end(),
                          [&along](Point2 p, Point2 q) { return along(p) < along(q); });
                for (std::size_t i = 1; i < cuts.size(); ++i) {
                    if (along(cuts[i]) - along(cuts[i - 1]) > joinTolerance * 10.0) {
                        Edge edge;
                        edge.curve = curve;
                        edge.start = cuts[i - 1];
                        edge.end = cuts[i];
                        edges.push_back(edge);
                    }
                }
                continue;
            }
            const Point2 center = position(item.center);
            const double radius = radiusOf(curve);
            const double base = item.kind == EntityKind::Arc
                                    ? std::atan2(position(item.start).y - center.y,
                                                 position(item.start).x - center.x)
                                    : 0.0;
            const auto angleOf = [&](Point2 p) {
                double angle = std::atan2(p.y - center.y, p.x - center.x) - base;
                while (angle < 0.0) {
                    angle += 2.0 * pi;
                }
                return angle;
            };
            if (item.kind == EntityKind::Arc) {
                cuts.push_back(position(item.start));
                cuts.push_back(position(item.end));
            } else if (cuts.size() < 2) {
                // A circle that is only touched: pieces all the same, for it has no ends.
                cuts.push_back({center.x + radius, center.y});
                cuts.push_back({center.x - radius, center.y});
            }
            std::sort(cuts.begin(), cuts.end(),
                      [&angleOf](Point2 p, Point2 q) { return angleOf(p) < angleOf(q); });
            const std::size_t pieces = item.kind == EntityKind::Arc ? cuts.size() - 1 : cuts.size();
            for (std::size_t i = 0; i < pieces; ++i) {
                const Point2 from = cuts[i];
                const Point2 to = cuts[(i + 1) % cuts.size()];
                if (std::hypot(from.x - to.x, from.y - to.y) > joinTolerance * 10.0) {
                    Edge edge;
                    edge.curve = curve;
                    edge.isArc = true;
                    edge.start = from;
                    edge.end = to;
                    edge.center = center;
                    edges.push_back(edge);
                }
            }
        }
        for (Edge& edge : edges) {
            edge.from = nodeAt(edge.start);
            edge.to = nodeAt(edge.end);
        }

        std::vector<std::vector<Point2>> polygonsHere;
        std::vector<Region> areas;
        for (const Walk& walk : walksOf(edges, nodes.size())) {
            Region area;
            area.area = walk.area;
            for (const std::size_t half : walk.halves) {
                const Edge& edge = edges[half / 2];
                area.pieces.push_back({edge.curve, edge.start, edge.end});
                area.outer.push_back(edge.curve);
            }
            std::sort(area.outer.begin(), area.outer.end());
            area.outer.erase(std::unique(area.outer.begin(), area.outer.end()), area.outer.end());
            // A point just inside, on the left of the way round, says which outlines it is in.
            const std::vector<Point2> polygon = polygonOf(walk, edges);
            const Point2 a = polygon[0];
            const Point2 b = polygon[1];
            const double length = std::hypot(b.x - a.x, b.y - a.y);
            const double nudge = std::min(length, 1.0) * 1e-3;
            const Point2 inside{(a.x + b.x) / 2.0 - (b.y - a.y) / length * nudge,
                                (a.y + b.y) / 2.0 + (b.x - a.x) / length * nudge};
            area.key = area.outer;
            area.key.push_back(0);
            for (const std::size_t i : group) {
                if (sideOf(polygons[i], inside) != Side::Outside) {
                    area.key.push_back(
                        *std::min_element(outlines[i].curves.begin(), outlines[i].curves.end()));
                }
            }
            std::sort(area.key.begin(), area.key.end());
            // Outlines of other groups that lie in it are holes in it, as for any region.
            for (std::size_t other = 0; other < count; ++other) {
                if (root(other) == first) {
                    continue;
                }
                const bool inHere =
                    std::all_of(polygons[other].begin(), polygons[other].end(),
                                [&polygon](Point2 p) { return sideOf(polygon, p) != Side::Outside; });
                if (!inHere) {
                    continue;
                }
                // Not if it lies in another outline that is in here too: that one is the hole.
                bool direct = true;
                for (std::size_t between = 0; direct && between < count; ++between) {
                    if (between != other && root(between) != first &&
                        outlines[between].area > outlines[other].area &&
                        std::all_of(polygons[between].begin(), polygons[between].end(),
                                    [&polygon](Point2 p) {
                                        return sideOf(polygon, p) != Side::Outside;
                                    }) &&
                        sideOf(polygons[between], polygons[other][0]) != Side::Outside) {
                        direct = false;
                    }
                }
                if (direct) {
                    area.holes.push_back(outlines[other].curves);
                    area.area -= outlines[other].area;
                }
            }
            areas.push_back(std::move(area));
        }
        std::sort(areas.begin(), areas.end(),
                  [](const Region& a, const Region& b) { return a.key < b.key; });
        found.insert(found.end(), areas.begin(), areas.end());
    }
    return found;
}

} // namespace lcad::sketch
