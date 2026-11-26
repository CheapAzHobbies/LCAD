// SPDX-License-Identifier: GPL-3.0-or-later

#include "Plan.h"

#include "MeshFacts.h"

#include <Eigen/Dense>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <numbers>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace lcad::recover {

namespace {

using Eigen::Vector2d;
using Eigen::Vector3d;
using kernel::MeshSurface;
using kernel::SurfaceKind;

constexpr double pi = std::numbers::pi;
// Two directions are one to within this: the sine of the angle between them.
constexpr double sameWay = 2e-4;
// What is left unexplained may be this share of the area, and is then simply left out; the check
// of the finished solid against the mesh has the last word.
constexpr double looseEnds = 0.004;

kernel::Point2D flat(const Vector2d& v) {
    return {v.x(), v.y()};
}

double cross2(const Vector2d& a, const Vector2d& b) {
    return a.x() * b.y() - a.y() * b.x();
}

// What a surface is, seen along one direction.
enum class Role {
    Side,  ///< runs along the direction: part of an outline
    Level, ///< square to it: where one layer ends and the next begins
    Blend, ///< a round or a bevel, left out of the outlines and put back after
    Hole,  ///< a hole drilled some other way
    Other, ///< fits none of it
};

// A stretch of a cut through the mesh that lies on one surface.
struct Run {
    Index surface = 0;
    std::vector<Vector2d> points;
};

// The surface a run lies on, in the sketch's plane.
struct Base {
    bool round = false;
    Vector2d through = Vector2d::Zero(); ///< a line: a point on it; a circle: its center
    Vector2d along = Vector2d::UnitX();
    double radius = 0.0;
};

class Planner {
public:
    Planner(const kernel::Mesh& mesh, const kernel::MeshSurfaces& found, std::string& why)
        : m_mesh(mesh), m_found(found), m_why(why), m_facts(mesh, found),
          m_points(m_facts.points()), m_area(m_facts.area()) {}

    std::optional<Plan> run() {
        if (!chooseDirection()) {
            return std::nullopt;
        }
        std::vector<double> levels = levelHeights();
        double low = std::numeric_limits<double>::max();
        double high = std::numeric_limits<double>::lowest();
        for (const Vector3d& p : m_points) {
            low = std::min(low, p.dot(m_way));
            high = std::max(high, p.dot(m_way));
        }
        const double close = m_found.tolerance * 4.0;
        if (levels.size() < 2 || std::abs(levels.front() - low) > close ||
            std::abs(levels.back() - high) > close) {
            m_why = "it has no flat top and bottom along any direction";
            return std::nullopt;
        }

        Plan plan;
        plan.base = kernel::makePlane({}, direction(m_way), direction(m_right));
        for (std::size_t i = 0; i + 1 < levels.size(); ++i) {
            std::optional<Layer> layer = layerBetween(levels[i], levels[i + 1]);
            if (!layer) {
                return std::nullopt;
            }
            plan.layers.push_back(std::move(*layer));
        }
        takeHoles(plan);
        sideHoles(plan);
        mergeLayers(plan);
        blends(plan);
        return plan;
    }

private:
    [[nodiscard]] Role roleOf(Index number, const Vector3d& way) const {
        const MeshSurface& s = m_found.surfaces[number];
        if (s.facet) {
            return Role::Other;
        }
        const double along = std::abs(vec(s.axis).dot(way));
        switch (s.kind) {
        case SurfaceKind::Plane:
            if (along > 1.0 - sameWay * sameWay) {
                return Role::Level;
            }
            if (along < sameWay) {
                return Role::Side;
            }
            return m_facts.isBevel(number) ? Role::Blend : Role::Other;
        case SurfaceKind::Cylinder:
            if (m_facts.isRound(number)) {
                return Role::Blend;
            }
            if (along > 1.0 - sameWay * sameWay) {
                return Role::Side;
            }
            return !s.outward && m_facts.sweepOf(number) > pi * 1.9 ? Role::Hole : Role::Other;
        case SurfaceKind::Torus:
            return m_facts.isRound(number) ? Role::Blend : Role::Other;
        case SurfaceKind::Sphere: {
            // Where three rounds meet.
            for (const auto& [other, contact] : m_facts.contacts(number)) {
                if (!m_facts.isRound(other)) {
                    return Role::Other;
                }
            }
            return Role::Blend;
        }
        case SurfaceKind::Cone:
            return along > 1.0 - sameWay * sameWay && m_facts.isBevel(number) ? Role::Blend : Role::Other;
        }
        return Role::Other;
    }

    bool chooseDirection() {
        // The directions of the faces, the one with the most on it first.
        std::vector<std::pair<double, Vector3d>> candidates;
        for (const MeshSurface& s : m_found.surfaces) {
            if (s.facet || (s.kind != SurfaceKind::Plane && s.kind != SurfaceKind::Cylinder)) {
                continue;
            }
            Vector3d way = vec(s.axis).normalized();
            bool known = false;
            for (auto& [area, candidate] : candidates) {
                if (std::abs(candidate.dot(way)) > 1.0 - sameWay * sameWay) {
                    area += s.area;
                    known = true;
                    break;
                }
            }
            if (!known) {
                candidates.push_back({s.area, way});
            }
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        double bestOther = std::numeric_limits<double>::max();
        const std::size_t tried = std::min<std::size_t>(candidates.size(), 8);
        for (std::size_t i = 0; i < tried; ++i) {
            Vector3d way = candidates[i].second;
            // Up, or the way the first axis it has any of goes.
            int largest = 0;
            way.cwiseAbs().maxCoeff(&largest);
            if (way[largest] < 0.0) {
                way = -way;
            }
            double other = 0.0;
            for (Index s = 0; s < m_found.surfaces.size(); ++s) {
                other += roleOf(s, way) == Role::Other ? m_found.surfaces[s].area : 0.0;
            }
            // Z breaks a tie: a part lies flat when it can.
            const bool better = other < bestOther - m_area * 1e-6 ||
                                (other < bestOther + m_area * 1e-6 && way.z() > m_way.z());
            if (better) {
                bestOther = other;
                m_way = way;
            }
        }
        if (candidates.empty() || bestOther > m_area * looseEnds) {
            m_why = "its faces do not line up along one direction";
            return false;
        }
        m_roles.resize(m_found.surfaces.size());
        for (Index s = 0; s < m_found.surfaces.size(); ++s) {
            m_roles[s] = roleOf(s, m_way);
        }
        // The right of the sketch: along X if it can be.
        const Vector3d x = std::abs(m_way.x()) < 0.9 ? Vector3d::UnitX() : Vector3d::UnitY();
        m_right = (x - m_way * x.dot(m_way)).normalized();
        m_up = m_way.cross(m_right);
        return true;
    }

    [[nodiscard]] std::vector<double> levelHeights() const {
        std::vector<double> levels;
        for (Index s = 0; s < m_found.surfaces.size(); ++s) {
            if (m_roles[s] == Role::Level) {
                levels.push_back(vec(m_found.surfaces[s].origin).dot(m_way));
            }
        }
        std::sort(levels.begin(), levels.end());
        const double close = m_found.tolerance * 4.0;
        levels.erase(std::unique(levels.begin(), levels.end(),
                                 [close](double a, double b) { return b - a < close; }),
                     levels.end());
        return levels;
    }

    // A cut through the mesh at a height along the direction: closed runs of segments, with the
    // solid on their left.
    [[nodiscard]] std::vector<std::vector<Run>> cutAt(double height) const {
        struct Segment {
            Vector2d from;
            Vector2d to;
            std::uint64_t next;
            Index surface;
            bool used = false;
        };
        std::vector<Segment> segments;
        std::unordered_map<std::uint64_t, std::size_t> startingAt;
        const auto keyOf = [](Index a, Index b) {
            return (std::uint64_t{std::min(a, b)} << 32U) | std::max(a, b);
        };
        std::vector<double> off(m_points.size());
        for (std::size_t i = 0; i < m_points.size(); ++i) {
            off[i] = m_points[i].dot(m_way) - height;
        }
        const auto in2d = [this](const Vector3d& p) {
            return Vector2d(p.dot(m_right), p.dot(m_up));
        };
        for (Index t = 0; t < m_mesh.triangles.size(); ++t) {
            const auto& c = m_mesh.triangles[t];
            std::array<std::pair<std::uint64_t, Vector2d>, 2> crossings;
            std::size_t count = 0;
            for (std::size_t k = 0; k < 3 && count < 2; ++k) {
                const Index a = c[k];
                const Index b = c[(k + 1) % 3];
                if ((off[a] >= 0.0) != (off[b] >= 0.0)) {
                    const double along = off[a] / (off[a] - off[b]);
                    crossings[count++] = {keyOf(a, b),
                                          in2d(m_points[a] + (m_points[b] - m_points[a]) * along)};
                }
            }
            if (count != 2) {
                continue;
            }
            const Vector3d normal =
                (m_points[c[1]] - m_points[c[0]]).cross(m_points[c[2]] - m_points[c[0]]);
            const Vector2d out(normal.dot(m_right), normal.dot(m_up));
            // With the outside on the right of the way it runs.
            const Vector2d way(-out.y(), out.x());
            const bool forward = (crossings[1].second - crossings[0].second).dot(way) >= 0.0;
            const auto& from = crossings[forward ? 0 : 1];
            const auto& to = crossings[forward ? 1 : 0];
            startingAt[from.first] = segments.size();
            segments.push_back({from.second, to.second, to.first, m_found.surfaceOf[t]});
        }
        std::vector<std::vector<Run>> loops;
        for (Segment& first : segments) {
            if (first.used) {
                continue;
            }
            std::vector<Run> runs;
            Segment* at = &first;
            while (at != nullptr && !at->used) {
                at->used = true;
                if (runs.empty() || runs.back().surface != at->surface) {
                    runs.push_back({at->surface, {at->from}});
                }
                runs.back().points.push_back(at->to);
                const auto found = startingAt.find(at->next);
                at = found == startingAt.end() ? nullptr : &segments[found->second];
            }
            if (runs.size() > 1 && runs.front().surface == runs.back().surface) {
                runs.front().points.insert(runs.front().points.begin(), runs.back().points.begin(),
                                           runs.back().points.end());
                runs.pop_back();
            }
            loops.push_back(std::move(runs));
        }
        return loops;
    }

    [[nodiscard]] Base baseOf(Index number) const {
        const MeshSurface& s = m_found.surfaces[number];
        const Vector3d on = vec(s.origin);
        Base base;
        if (s.kind == SurfaceKind::Plane) {
            const Vector3d normal = vec(s.axis);
            const Vector2d flatNormal(normal.dot(m_right), normal.dot(m_up));
            base.through = Vector2d(on.dot(m_right), on.dot(m_up));
            base.along = Vector2d(-flatNormal.y(), flatNormal.x()).normalized();
            return base;
        }
        base.round = true;
        base.through = Vector2d(on.dot(m_right), on.dot(m_up));
        base.radius = s.radius;
        return base;
    }

    // Where two curves meet, the nearest place to `near`.
    [[nodiscard]] static std::optional<Vector2d> meeting(const Base& a, const Base& b,
                                                         const Vector2d& nearTo) {
        std::vector<Vector2d> found;
        if (!a.round && !b.round) {
            const double across = cross2(a.along, b.along);
            if (std::abs(across) < 1e-9) {
                return std::nullopt;
            }
            const double t = cross2(b.through - a.through, b.along) / across;
            return Vector2d(a.through + a.along * t);
        }
        if (a.round != b.round) {
            const Base& line = a.round ? b : a;
            const Base& circle = a.round ? a : b;
            const Vector2d from = line.through - circle.through;
            const double along = -from.dot(line.along);
            const Vector2d foot = line.through + line.along * along;
            const double distance = (foot - circle.through).norm();
            const double left = circle.radius * circle.radius - distance * distance;
            // Tangent, to within rounding.
            if (left <= circle.radius * circle.radius * 1e-5) {
                return foot;
            }
            const double half = std::sqrt(left);
            found = {foot + line.along * half, foot - line.along * half};
        } else {
            const Vector2d apart = b.through - a.through;
            const double d = apart.norm();
            if (d < 1e-9) {
                return std::nullopt;
            }
            const double along = (d * d + a.radius * a.radius - b.radius * b.radius) / (2.0 * d);
            const double left = a.radius * a.radius - along * along;
            const Vector2d unit = apart / d;
            const Vector2d foot = a.through + unit * along;
            if (left <= a.radius * a.radius * 1e-5) {
                return foot;
            }
            const Vector2d side(-unit.y(), unit.x());
            found = {foot + side * std::sqrt(left), foot - side * std::sqrt(left)};
        }
        return (found[0] - nearTo).squaredNorm() < (found[1] - nearTo).squaredNorm() ? found[0]
                                                                                     : found[1];
    }

    // A cut as outlines of exact curves. Nothing if something in it is not a side.
    [[nodiscard]] std::optional<std::vector<Outline>> outlinesAt(double height,
                                                                 std::vector<PlannedEdge>& edges) const {
        std::vector<Outline> outlines;
        for (const std::vector<Run>& loop : cutAt(height)) {
            double area = 0.0;
            for (const Run& run : loop) {
                for (std::size_t i = 1; i < run.points.size(); ++i) {
                    area += cross2(run.points[i - 1], run.points[i]) / 2.0;
                }
            }
            // What is on the sides stays, and what lies between runs becomes a corner.
            std::vector<const Run*> kept;
            std::vector<double> roundedBefore;
            double rounded = 0.0;
            for (const Run& run : loop) {
                const Role role = m_roles[run.surface];
                if (role == Role::Side) {
                    if (!kept.empty() && kept.back()->surface == run.surface) {
                        continue;
                    }
                    kept.push_back(&run);
                    roundedBefore.push_back(rounded);
                    rounded = 0.0;
                } else if (role == Role::Blend) {
                    const MeshSurface& s = m_found.surfaces[run.surface];
                    if (s.kind == SurfaceKind::Cylinder &&
                        std::abs(vec(s.axis).dot(m_way)) > 1.0 - sameWay * sameWay) {
                        rounded = s.radius;
                    }
                } else if (role != Role::Hole && !m_found.surfaces[run.surface].facet) {
                    return std::nullopt;
                }
            }
            if (kept.empty()) {
                continue; // a hole from the side, seen from within
            }
            if (rounded > 0.0) {
                roundedBefore.front() = rounded;
            }
            if (kept.size() > 1 && kept.front()->surface == kept.back()->surface) {
                kept.pop_back();
            }
            Outline outline;
            outline.area = area;
            const std::size_t count = kept.size();
            if (count == 1) {
                const Base base = baseOf(kept[0]->surface);
                if (!base.round) {
                    return std::nullopt;
                }
                Curve whole;
                whole.round = true;
                whole.whole = true;
                whole.center = flat(base.through);
                whole.radius = base.radius;
                outline.curves.push_back(whole);
            } else {
                std::vector<Vector2d> corners(count);
                for (std::size_t i = 0; i < count; ++i) {
                    const std::size_t next = (i + 1) % count;
                    const auto corner = meeting(baseOf(kept[i]->surface), baseOf(kept[next]->surface),
                                                (kept[i]->points.back() + kept[next]->points.front()) / 2.0);
                    if (!corner) {
                        return std::nullopt;
                    }
                    corners[i] = *corner;
                    if (roundedBefore[next] > 0.0) {
                        const Vector3d at = m_right * corner->x() + m_up * corner->y() + m_way * height;
                        edges.push_back({point(at), roundedBefore[next], false});
                    }
                }
                for (std::size_t i = 0; i < count; ++i) {
                    const Base base = baseOf(kept[i]->surface);
                    Curve curve;
                    curve.start = flat(corners[(i + count - 1) % count]);
                    curve.end = flat(corners[i]);
                    if (base.round) {
                        curve.round = true;
                        curve.center = flat(base.through);
                        curve.radius = base.radius;
                        const std::vector<Vector2d>& along = kept[i]->points;
                        const Vector2d a = along.front() - base.through;
                        const Vector2d b = along[along.size() / 2] - base.through;
                        curve.counterClockwise = cross2(a, b) > 0.0;
                    }
                    outline.curves.push_back(curve);
                }
            }
            outline.signature = signatureOf(outline);
            outlines.push_back(std::move(outline));
        }
        return outlines;
    }

    [[nodiscard]] static std::string signatureOf(const Outline& outline) {
        std::vector<std::string> parts;
        char text[120];
        for (const Curve& curve : outline.curves) {
            std::snprintf(text, sizeof text, "%d %.3f %.3f %.3f %.3f %.3f", curve.round ? 1 : 0,
                          curve.whole ? curve.center.x : curve.start.x,
                          curve.whole ? curve.center.y : curve.start.y, curve.end.x * !curve.whole,
                          curve.end.y * !curve.whole, curve.radius);
            parts.emplace_back(text);
        }
        std::rotate(parts.begin(), std::min_element(parts.begin(), parts.end()), parts.end());
        std::string out = outline.area > 0.0 ? "+" : "-";
        for (const std::string& part : parts) {
            out += part + ";";
        }
        return out;
    }

    [[nodiscard]] std::optional<Layer> layerBetween(double low, double high) {
        // Somewhere it is all sides: a round at the rim can reach well into a thin layer.
        std::string reason;
        for (const double share : {0.5, 0.37, 0.63, 0.21, 0.79, 0.09, 0.91}) {
            std::vector<PlannedEdge> edges;
            auto outlines = outlinesAt(low + (high - low) * share, edges);
            if (outlines) {
                Layer layer;
                layer.low = low;
                layer.high = high;
                layer.outlines = std::move(*outlines);
                m_cornerEdges.insert(m_cornerEdges.end(), edges.begin(), edges.end());
                return layer;
            }
        }
        m_why = "a cut through it does not come out as lines and arcs";
        return std::nullopt;
    }

    // Round holes that open at the top or the bottom are drilled; the rest stay in the outlines.
    void takeHoles(Plan& plan) const {
        std::map<std::string, std::vector<std::size_t>> holes;
        for (std::size_t i = 0; i < plan.layers.size(); ++i) {
            for (const Outline& outline : plan.layers[i].outlines) {
                if (outline.area < 0.0 && outline.curves.size() == 1 && outline.curves[0].whole) {
                    holes[outline.signature].push_back(i);
                }
            }
        }
        std::vector<std::pair<std::size_t, std::string>> taken;
        for (const auto& [signature, layers] : holes) {
            const std::size_t first = layers.front();
            const std::size_t last = layers.back();
            if (last - first + 1 != layers.size()) {
                continue;
            }
            const bool top = last + 1 == plan.layers.size();
            const bool bottom = first == 0;
            if (!top && !bottom) {
                continue;
            }
            const Curve* circle = nullptr;
            for (const Outline& outline : plan.layers[first].outlines) {
                if (outline.signature == signature) {
                    circle = &outline.curves[0];
                }
            }
            PlannedHole hole;
            const double from = top ? plan.layers[last].high : plan.layers[first].low;
            const Vector3d out = top ? m_way : Vector3d(-m_way);
            const Vector3d origin = m_way * from;
            hole.plane = kernel::makePlane(point(origin), direction(out), direction(m_right));
            hole.at = {circle->center.x, top ? circle->center.y : -circle->center.y};
            hole.diameter = circle->radius * 2.0;
            hole.depth = plan.layers[last].high - plan.layers[first].low;
            hole.through = top && bottom;
            plan.holes.push_back(hole);
            for (std::size_t i = first; i <= last; ++i) {
                taken.push_back({i, signature});
            }
        }
        for (const auto& [layer, signature] : taken) {
            std::erase_if(plan.layers[layer].outlines,
                          [&signature](const Outline& o) { return o.signature == signature; });
        }
    }

    void sideHoles(Plan& plan) const {
        for (Index number = 0; number < m_found.surfaces.size(); ++number) {
            if (m_roles[number] != Role::Hole) {
                continue;
            }
            const MeshSurface& s = m_found.surfaces[number];
            const Vector3d axis = vec(s.axis).normalized();
            const Vector3d on = vec(s.origin);
            double low = std::numeric_limits<double>::max();
            double high = std::numeric_limits<double>::lowest();
            for (const Index t : s.triangles) {
                for (const Index c : m_mesh.triangles[t]) {
                    const double along = (m_points[c] - on).dot(axis);
                    low = std::min(low, along);
                    high = std::max(high, along);
                }
            }
            const Vector3d helper =
                std::abs(axis.x()) < 0.9 ? Vector3d(1.0, 0.0, 0.0) : Vector3d(0.0, 1.0, 0.0);
            const Vector3d x = axis.cross(helper).normalized();
            PlannedHole hole;
            hole.plane = kernel::makePlane(point(on + axis * high), direction(axis), direction(x));
            hole.diameter = s.radius * 2.0;
            hole.depth = high - low;
            plan.holes.push_back(hole);
        }
    }

    static void mergeLayers(Plan& plan) {
        const auto sameOutlines = [](const Layer& a, const Layer& b) {
            std::vector<std::string> first;
            std::vector<std::string> second;
            for (const Outline& o : a.outlines) {
                first.push_back(o.signature);
            }
            for (const Outline& o : b.outlines) {
                second.push_back(o.signature);
            }
            std::sort(first.begin(), first.end());
            std::sort(second.begin(), second.end());
            return first == second;
        };
        std::vector<Layer> merged;
        for (Layer& layer : plan.layers) {
            if (!merged.empty() && sameOutlines(merged.back(), layer)) {
                merged.back().high = layer.high;
            } else {
                merged.push_back(std::move(layer));
            }
        }
        plan.layers = std::move(merged);
    }

    void blends(Plan& plan) const {
        plan.edges = m_cornerEdges;
        for (Index number = 0; number < m_found.surfaces.size(); ++number) {
            if (m_roles[number] != Role::Blend) {
                continue;
            }
            const MeshSurface& s = m_found.surfaces[number];
            if (s.kind == SurfaceKind::Sphere) {
                continue;
            }
            if (s.kind == SurfaceKind::Plane || s.kind == SurfaceKind::Cone) {
                bevel(plan, number);
                continue;
            }
            // Upright rounds were seen in the outlines.
            if (s.kind == SurfaceKind::Cylinder &&
                std::abs(vec(s.axis).dot(m_way)) > 1.0 - sameWay * sameWay) {
                continue;
            }
            std::vector<Index> sides;
            for (const Index other : m_facts.tangentTo(number)) {
                if (m_roles[other] != Role::Blend) {
                    sides.push_back(other);
                }
            }
            if (sides.size() < 2) {
                continue;
            }
            const double size = s.kind == SurfaceKind::Torus ? s.tubeRadius : s.radius;
            const auto at = m_facts.meeting(m_facts.pointOn(number), {sides[0], sides[1]}, size * 8.0);
            if (at) {
                plan.edges.push_back({point(*at), size, false});
            }
        }
        // Largest first: a round runs out into a smaller one more readily than the other way.
        std::stable_sort(plan.edges.begin(), plan.edges.end(),
                         [](const PlannedEdge& a, const PlannedEdge& b) {
                             return std::pair(a.chamfer, std::lround(a.size * 1000.0)) >
                                    std::pair(b.chamfer, std::lround(b.size * 1000.0));
                         });
    }

    void bevel(Plan& plan, Index number) const {
        const MeshSurface& s = m_found.surfaces[number];
        std::vector<std::pair<double, Index>> around;
        for (const auto& [other, contact] : m_facts.contacts(number)) {
            around.push_back({contact.length, other});
        }
        std::sort(around.rbegin(), around.rend());
        const Index first = around[0].second;
        const Index second = around[1].second;
        const auto meets = m_facts.meeting(m_facts.pointOn(number), {first, second}, m_facts.size());
        if (!meets) {
            return;
        }
        const Vector3d at = *meets;
        const MeshSurface& a = m_found.surfaces[first];
        const MeshSurface& b = m_found.surfaces[second];
        const Vector3d normal = gradientOf(s, at);
        const double off = std::abs(offsetFrom(s, at));
        const Vector3d edge = gradientOf(a, at).cross(gradientOf(b, at)).normalized();
        const double acrossA = std::abs(edge.cross(gradientOf(a, at)).dot(normal));
        if (acrossA < 1e-6) {
            return;
        }
        plan.edges.push_back({point(at), off / acrossA, true});
    }

    const kernel::Mesh& m_mesh;
    const kernel::MeshSurfaces& m_found;
    std::string& m_why;
    MeshFacts m_facts;
    const std::vector<Vector3d>& m_points;
    double m_area = 0.0;
    std::vector<Role> m_roles;
    Vector3d m_way = Vector3d::UnitZ();
    Vector3d m_right = Vector3d::UnitX();
    Vector3d m_up = Vector3d::UnitY();
    std::vector<PlannedEdge> m_cornerEdges;
};

} // namespace

std::optional<Plan> planPart(const kernel::Mesh& mesh, const kernel::MeshSurfaces& found,
                             std::string& why) {
    return Planner(mesh, found, why).run();
}

} // namespace lcad::recover
