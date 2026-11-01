// SPDX-License-Identifier: GPL-3.0-or-later

#include "Sharpen.h"

#include "MeshFacts.h"

#include <lcad/kernel/MeshSurfaces.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace lcad::recover {

namespace {

using Eigen::Vector3d;
using kernel::Mesh;
using kernel::MeshSurface;
using kernel::SurfaceKind;

constexpr double pi = std::numbers::pi;
// Nothing has more steps than this.
constexpr int mostPasses = 64;
// A hole's wall goes all the way around, to within this share.
constexpr double wholeTurn = 0.98;
// A round's corners move at most this many of its radii: to an edge of some 15 degrees.
constexpr double furthest = 8.0;
// The two sides of a bevel Chamfer can make are this close to each other, as a share.
constexpr double evenBevel = 0.03;
// A round has a corner under it: faces at this many degrees. Less is a curve that flows into the
// next, as in the outline of a letter.
constexpr double leastCorner = 15.0;

std::uint64_t sideKey(Index from, Index to) {
    return (std::uint64_t{from} << 32U) | to;
}

// Whether each side of each triangle has the other side there, running the other way.
bool isClosed(const Mesh& mesh) {
    std::unordered_map<std::uint64_t, int> sides;
    sides.reserve(mesh.triangles.size() * 3);
    for (const auto& c : mesh.triangles) {
        for (std::size_t k = 0; k < 3; ++k) {
            const Index a = c[k];
            const Index b = c[(k + 1) % 3];
            ++sides[a < b ? sideKey(a, b) : sideKey(b, a)];
            sides[a < b ? sideKey(b, a) : sideKey(a, b)] += 0;
        }
    }
    for (const auto& c : mesh.triangles) {
        for (std::size_t k = 0; k < 3; ++k) {
            const Index a = c[k];
            const Index b = c[(k + 1) % 3];
            if (sides[a < b ? sideKey(a, b) : sideKey(b, a)] % 2 != 0) {
                return false;
            }
        }
    }
    return true;
}

double areaOf(const Mesh& mesh, const std::array<Index, 3>& c) {
    const Vector3d p = vec(mesh.vertices[c[0]]);
    return (vec(mesh.vertices[c[1]]) - p).cross(vec(mesh.vertices[c[2]]) - p).norm() / 2.0;
}

// Corners in one place become one, what has collapsed goes, and so do corners nothing uses and
// surfaces nothing lies on.
void tidy(Mesh& mesh, kernel::MeshSurfaces& found, double close) {
    std::map<std::array<std::int64_t, 3>, Index> known;
    std::vector<Index> number(mesh.vertices.size());
    std::vector<kernel::Point> kept;
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const kernel::Point& p = mesh.vertices[i];
        const std::array<std::int64_t, 3> key{std::llround(p.x / close), std::llround(p.y / close),
                                              std::llround(p.z / close)};
        const auto [place, isNew] = known.try_emplace(key, static_cast<Index>(kept.size()));
        if (isNew) {
            kept.push_back(p);
        }
        number[i] = place->second;
    }
    const auto turned = [](std::array<Index, 3> c) {
        std::rotate(c.begin(), std::min_element(c.begin(), c.end()), c.end());
        return c;
    };
    // Two that lie back to back, as a fold in something collapsed: both go.
    std::map<std::array<Index, 3>, std::vector<std::size_t>> faces;
    std::vector<bool> stays(mesh.triangles.size(), true);
    for (std::size_t t = 0; t < mesh.triangles.size(); ++t) {
        const auto& c = mesh.triangles[t];
        const std::array<Index, 3> corners{number[c[0]], number[c[1]], number[c[2]]};
        if (corners[0] == corners[1] || corners[1] == corners[2] || corners[2] == corners[0]) {
            stays[t] = false;
            continue;
        }
        std::vector<std::size_t>& other = faces[turned({corners[0], corners[2], corners[1]})];
        if (!other.empty()) {
            stays[other.back()] = false;
            stays[t] = false;
            other.pop_back();
            continue;
        }
        faces[turned(corners)].push_back(t);
    }
    std::vector<std::array<Index, 3>> triangles;
    std::vector<Index> surfaceOf;
    std::vector<Index> used(kept.size(), std::numeric_limits<Index>::max());
    std::vector<kernel::Point> vertices;
    for (std::size_t t = 0; t < mesh.triangles.size(); ++t) {
        if (!stays[t]) {
            continue;
        }
        std::array<Index, 3> corners{};
        for (std::size_t k = 0; k < 3; ++k) {
            Index& place = used[number[mesh.triangles[t][k]]];
            if (place == std::numeric_limits<Index>::max()) {
                place = static_cast<Index>(vertices.size());
                vertices.push_back(kept[number[mesh.triangles[t][k]]]);
            }
            corners[k] = place;
        }
        triangles.push_back(corners);
        surfaceOf.push_back(found.surfaceOf[t]);
    }
    mesh.vertices = std::move(vertices);
    mesh.triangles = std::move(triangles);

    std::vector<MeshSurface> surfaces;
    std::vector<Index> renumbered(found.surfaces.size(), std::numeric_limits<Index>::max());
    double all = 0.0;
    double real = 0.0;
    for (Index t = 0; t < mesh.triangles.size(); ++t) {
        const Index old = surfaceOf[t];
        if (renumbered[old] == std::numeric_limits<Index>::max()) {
            renumbered[old] = static_cast<Index>(surfaces.size());
            surfaces.push_back(found.surfaces[old]);
            surfaces.back().triangles.clear();
            surfaces.back().area = 0.0;
        }
        MeshSurface& surface = surfaces[renumbered[old]];
        surfaceOf[t] = renumbered[old];
        surface.triangles.push_back(t);
        const double area = areaOf(mesh, mesh.triangles[t]);
        surface.area += area;
        all += area;
        real += surface.facet ? 0.0 : area;
    }
    found.surfaces = std::move(surfaces);
    found.surfaceOf = std::move(surfaceOf);
    found.share = all > 0.0 ? real / all : 0.0;
}

// Rounds of one radius that run into each other, or a bevel with those beside it.
struct Group {
    std::vector<Index> members;
    double size = 0.0;
    bool chamfer = false;
};

class Pass {
public:
    Pass(Mesh& mesh, kernel::MeshSurfaces& found, const std::vector<kernel::Point>& refused)
        : m_mesh(mesh), m_found(found), m_refused(refused), m_facts(mesh, found),
          m_touching(mesh.vertices.size()) {
        for (Index t = 0; t < mesh.triangles.size(); ++t) {
            for (const Index c : mesh.triangles[t]) {
                std::vector<Index>& surfaces = m_touching[c];
                if (std::find(surfaces.begin(), surfaces.end(), found.surfaceOf[t]) == surfaces.end()) {
                    surfaces.push_back(found.surfaceOf[t]);
                }
            }
        }
    }

    // Takes off the smallest of what can be taken off: all of that size.
    bool edges(std::vector<PlannedEdge>& taken) {
        std::vector<Group> groups = roundGroups();
        const std::vector<Group> bevels = bevelGroups();
        groups.insert(groups.end(), bevels.begin(), bevels.end());
        std::stable_sort(groups.begin(), groups.end(),
                         [](const Group& a, const Group& b) { return a.size < b.size; });
        std::set<Index> moved;
        double size = -1.0;
        for (const Group& group : groups) {
            if (size >= 0.0 && group.size > size + m_found.tolerance * 4.0) {
                break;
            }
            if (take(group, moved, taken) && size < 0.0) {
                size = group.size;
            }
        }
        return size >= 0.0;
    }

    // Takes out the holes that close flat, the smallest first.
    bool holes(std::vector<PlannedHole>& taken) {
        std::vector<bool> gone(m_mesh.triangles.size(), false);
        std::vector<std::array<Index, 3>> added;
        std::vector<Index> addedOn;
        std::vector<Index> walls;
        for (Index s = 0; s < m_found.surfaces.size(); ++s) {
            const MeshSurface& surface = m_found.surfaces[s];
            if (!surface.facet && surface.kind == SurfaceKind::Cylinder && !surface.outward &&
                m_facts.sweepOf(s) > 2.0 * pi * wholeTurn) {
                walls.push_back(s);
            }
        }
        std::sort(walls.begin(), walls.end(), [this](Index a, Index b) {
            return m_found.surfaces[a].radius < m_found.surfaces[b].radius;
        });
        bool any = false;
        for (const Index wall : walls) {
            any = takeHole(wall, gone, added, addedOn, taken) || any;
        }
        if (!any) {
            return false;
        }
        std::vector<std::array<Index, 3>> kept;
        std::vector<Index> keptOn;
        kept.reserve(m_mesh.triangles.size() + added.size());
        for (std::size_t t = 0; t < m_mesh.triangles.size(); ++t) {
            if (!gone[t]) {
                kept.push_back(m_mesh.triangles[t]);
                keptOn.push_back(m_found.surfaceOf[t]);
            }
        }
        kept.insert(kept.end(), added.begin(), added.end());
        keptOn.insert(keptOn.end(), addedOn.begin(), addedOn.end());
        m_mesh.triangles = std::move(kept);
        m_found.surfaceOf = std::move(keptOn);
        return true;
    }

private:
    [[nodiscard]] double sizeOf(Index number) const {
        const MeshSurface& s = m_found.surfaces[number];
        return s.kind == SurfaceKind::Torus ? s.tubeRadius : s.radius;
    }

    [[nodiscard]] bool sameSize(double a, double b) const {
        return std::abs(a - b) <= std::max(m_found.tolerance * 4.0, std::max(a, b) * 1e-3);
    }

    [[nodiscard]] std::vector<Group> roundGroups() const {
        std::vector<bool> seen(m_found.surfaces.size(), false);
        std::vector<Group> groups;
        for (Index s = 0; s < m_found.surfaces.size(); ++s) {
            if (seen[s] || !m_facts.isRound(s)) {
                continue;
            }
            Group group;
            group.size = sizeOf(s);
            std::vector<Index> open{s};
            seen[s] = true;
            while (!open.empty()) {
                const Index at = open.back();
                open.pop_back();
                group.members.push_back(at);
                for (const Index other : m_facts.tangentTo(at)) {
                    const MeshSurface& next = m_found.surfaces[other];
                    // A ball where three rounds meet goes with them.
                    if (!seen[other] && !next.facet && sameSize(sizeOf(other), group.size) &&
                        (m_facts.isRound(other) || next.kind == SurfaceKind::Sphere)) {
                        seen[other] = true;
                        open.push_back(other);
                    }
                }
            }
            groups.push_back(std::move(group));
        }
        return groups;
    }

    [[nodiscard]] bool isEvenBevel(Index number) const {
        // Mostly two long sides: a strip.
        double longest = 0.0;
        double second = 0.0;
        double total = 0.0;
        for (const auto& [other, contact] : m_facts.contacts(number)) {
            total += contact.length;
            if (contact.tangent > contact.length * 0.5) {
                return false;
            }
            if (contact.length > longest) {
                second = longest;
                longest = contact.length;
            } else {
                second = std::max(second, contact.length);
            }
        }
        return m_facts.isBevel(number) && longest + second >= total * 0.6;
    }

    [[nodiscard]] std::vector<Group> bevelGroups() const {
        std::vector<bool> seen(m_found.surfaces.size(), false);
        std::vector<Group> groups;
        for (Index s = 0; s < m_found.surfaces.size(); ++s) {
            if (seen[s] || !isEvenBevel(s)) {
                continue;
            }
            Group group;
            group.chamfer = true;
            group.size = m_facts.widthOf(s);
            std::vector<Index> open{s};
            seen[s] = true;
            while (!open.empty()) {
                const Index at = open.back();
                open.pop_back();
                group.members.push_back(at);
                for (const auto& [other, contact] : m_facts.contacts(at)) {
                    if (!seen[other] && isEvenBevel(other) &&
                        std::abs(m_facts.widthOf(other) - group.size) <= group.size * evenBevel) {
                        seen[other] = true;
                        open.push_back(other);
                    }
                }
            }
            groups.push_back(std::move(group));
        }
        return groups;
    }

    // The faces a member lies between, the longest contact first.
    [[nodiscard]] std::vector<Index> supportsOf(Index member, const Group& group) const {
        const auto inGroup = [&group](Index s) {
            return std::find(group.members.begin(), group.members.end(), s) != group.members.end();
        };
        std::vector<Index> supports;
        if (group.chamfer) {
            std::vector<std::pair<double, Index>> around;
            for (const auto& [other, contact] : m_facts.contacts(member)) {
                if (!inGroup(other) && !m_found.surfaces[other].facet) {
                    around.push_back({contact.length, other});
                }
            }
            std::sort(around.rbegin(), around.rend());
            for (std::size_t i = 0; i < std::min<std::size_t>(around.size(), 2); ++i) {
                supports.push_back(around[i].second);
            }
            return supports;
        }
        for (const Index other : m_facts.tangentTo(member)) {
            if (!inGroup(other) && !m_found.surfaces[other].facet) {
                supports.push_back(other);
            }
        }
        // A ball has only rounds around it: it comes to the faces they lie between.
        if (m_found.surfaces[member].kind == SurfaceKind::Sphere) {
            for (const Index other : m_facts.tangentTo(member)) {
                if (!inGroup(other)) {
                    continue;
                }
                for (const Index beyond : m_facts.tangentTo(other)) {
                    if (!inGroup(beyond) && !m_found.surfaces[beyond].facet &&
                        std::find(supports.begin(), supports.end(), beyond) == supports.end()) {
                        supports.push_back(beyond);
                    }
                }
            }
        }
        return supports;
    }

    // Where a bevel's two sides lie from the sharp edge: the same on both, or nothing.
    [[nodiscard]] std::optional<double> setback(Index member, const Vector3d& at,
                                                const std::vector<Index>& supports) const {
        const MeshSurface& s = m_found.surfaces[member];
        const MeshSurface& a = m_found.surfaces[supports[0]];
        const MeshSurface& b = m_found.surfaces[supports[1]];
        const Vector3d normal = gradientOf(s, at);
        const double off = std::abs(offsetFrom(s, at));
        const Vector3d ga = gradientOf(a, at);
        const Vector3d gb = gradientOf(b, at);
        const Vector3d edge = ga.cross(gb);
        if (edge.norm() < 1e-6) {
            return std::nullopt;
        }
        const Vector3d line = edge.normalized();
        const double acrossA = std::abs(line.cross(ga).dot(normal));
        const double acrossB = std::abs(line.cross(gb).dot(normal));
        if (acrossA < 1e-6 || acrossB < 1e-6) {
            return std::nullopt;
        }
        const double first = off / acrossA;
        const double second = off / acrossB;
        if (std::abs(first - second) > first * evenBevel) {
            return std::nullopt;
        }
        return (first + second) / 2.0;
    }

    bool take(const Group& group, std::set<Index>& moved, std::vector<PlannedEdge>& taken) {
        const auto inGroup = [&group](Index s) {
            return std::find(group.members.begin(), group.members.end(), s) != group.members.end();
        };
        std::map<Index, std::vector<Index>> supports;
        std::vector<PlannedEdge> found;
        double reach = group.size * furthest;
        for (const Index member : group.members) {
            std::vector<Index> around = supportsOf(member, group);
            supports[member] = around;
            if (m_found.surfaces[member].kind == SurfaceKind::Sphere) {
                continue;
            }
            if (around.size() < 2) {
                return false;
            }
            const auto at = m_facts.meeting(m_facts.pointOn(member), {around[0], around[1]},
                                            group.chamfer ? m_facts.size() : reach);
            if (!at) {
                return false;
            }
            const double cosine = facing(m_found.surfaces[around[0]], *at)
                                      .dot(facing(m_found.surfaces[around[1]], *at));
            if (cosine > std::cos(leastCorner * pi / 180.0)) {
                return false;
            }
            if (std::any_of(m_refused.begin(), m_refused.end(), [&](const kernel::Point& p) {
                    return (vec(p) - *at).norm() < std::max(m_found.tolerance * 8.0, 1e-3);
                })) {
                return false;
            }
            double size = group.size;
            if (group.chamfer) {
                const auto back = setback(member, *at, around);
                if (!back) {
                    return false;
                }
                size = *back;
                reach = size * furthest;
            }
            found.push_back({point(*at), size, group.chamfer});
        }

        std::vector<std::pair<Index, Vector3d>> moves;
        std::set<Index> corners;
        for (const Index member : group.members) {
            for (const Index t : m_found.surfaces[member].triangles) {
                corners.insert(m_mesh.triangles[t].begin(), m_mesh.triangles[t].end());
            }
        }
        for (const Index corner : corners) {
            if (moved.count(corner) != 0) {
                return false; // it comes up in the next pass
            }
            std::vector<Index> on;
            const auto add = [&on](Index s) {
                if (std::find(on.begin(), on.end(), s) == on.end()) {
                    on.push_back(s);
                }
            };
            for (const Index s : m_touching[corner]) {
                if (inGroup(s)) {
                    for (const Index support : supports[s]) {
                        add(support);
                    }
                }
            }
            // A corner stays on all that it was on: every surface goes on being what it was.
            for (const Index s : m_touching[corner]) {
                if (!inGroup(s)) {
                    add(s);
                }
            }
            if (on.size() < 2) {
                return false;
            }
            const std::optional<Vector3d> to = m_facts.meeting(m_facts.points()[corner], on, reach);
            if (!to) {
                return false;
            }
            moves.push_back({corner, *to});
        }
        for (const auto& [corner, to] : moves) {
            m_mesh.vertices[corner] = point(to);
            moved.insert(corner);
        }
        // What is left of its triangles lies in the sharp edge, and so on what it lay between.
        for (const Index member : group.members) {
            for (const Index t : m_found.surfaces[member].triangles) {
                m_found.surfaceOf[t] = supports[member].front();
            }
        }
        taken.insert(taken.end(), found.begin(), found.end());
        return true;
    }

    bool takeHole(Index wall, std::vector<bool>& gone, std::vector<std::array<Index, 3>>& added,
                  std::vector<Index>& addedOn, std::vector<PlannedHole>& taken) const {
        const MeshSurface& s = m_found.surfaces[wall];
        const Vector3d axis = vec(s.axis).normalized();
        const Vector3d on = vec(s.origin);
        double low = std::numeric_limits<double>::max();
        double high = std::numeric_limits<double>::lowest();
        for (const Index t : s.triangles) {
            for (const Index c : m_mesh.triangles[t]) {
                const double along = (m_facts.points()[c] - on).dot(axis);
                low = std::min(low, along);
                high = std::max(high, along);
            }
        }
        // What closes it: a floor or a drill's point, with nothing around it but the wall.
        std::vector<Index> removed{wall};
        bool closedLow = false;
        bool closedHigh = false;
        double tipAngle = 180.0;
        for (const auto& [other, contact] : m_facts.contacts(wall)) {
            const MeshSurface& cap = m_found.surfaces[other];
            if (m_facts.contacts(other).size() != 1 || cap.facet) {
                continue;
            }
            const bool floor = cap.kind == SurfaceKind::Plane &&
                               std::abs(vec(cap.axis).dot(axis)) > 1.0 - 1e-6;
            const bool tip = cap.kind == SurfaceKind::Cone &&
                             std::abs(vec(cap.axis).dot(axis)) > 1.0 - 1e-6;
            if (!floor && !tip) {
                continue;
            }
            const double along = (m_facts.pointOn(other) - on).dot(axis);
            (along < (low + high) / 2.0 ? closedLow : closedHigh) = true;
            if (tip) {
                tipAngle = cap.halfAngle * 2.0 * 180.0 / pi;
            }
            removed.push_back(other);
        }
        if (closedLow && closedHigh) {
            return false; // a hollow
        }

        // The rims it leaves, each running around the way the wall's triangles do.
        std::set<Index> leaving(removed.begin(), removed.end());
        std::unordered_map<std::uint64_t, Index> ownSides;
        std::vector<Index> theTriangles;
        for (const Index number : removed) {
            for (const Index t : m_found.surfaces[number].triangles) {
                if (gone[t]) {
                    return false; // another hole in this pass runs into it
                }
                theTriangles.push_back(t);
                const auto& c = m_mesh.triangles[t];
                for (std::size_t k = 0; k < 3; ++k) {
                    ownSides[sideKey(c[k], c[(k + 1) % 3])] = t;
                }
            }
        }
        std::map<Index, Index> next;
        for (const auto& [key, t] : ownSides) {
            const auto from = static_cast<Index>(key >> 32U);
            const auto to = static_cast<Index>(key & 0xFFFFFFFFU);
            if (ownSides.count(sideKey(to, from)) == 0) {
                if (next.count(from) != 0) {
                    return false; // rims that touch
                }
                next[from] = to;
            }
        }
        if (next.empty()) {
            return false;
        }
        // Every rim lies on one flat face.
        std::vector<std::vector<Index>> rims;
        std::vector<Index> rimsOn;
        std::set<Index> walked;
        for (const auto& [start, unused] : next) {
            if (walked.count(start) != 0) {
                continue;
            }
            std::vector<Index> rim;
            Index at = start;
            while (walked.insert(at).second) {
                rim.push_back(at);
                const auto found = next.find(at);
                if (found == next.end()) {
                    return false;
                }
                at = found->second;
            }
            if (at != start || rim.size() < 3) {
                return false;
            }
            std::optional<Index> flat;
            for (const Index corner : rim) {
                for (const Index surface : m_touching[corner]) {
                    if (leaving.count(surface) != 0) {
                        continue;
                    }
                    if (m_found.surfaces[surface].kind != SurfaceKind::Plane ||
                        m_found.surfaces[surface].facet || (flat && *flat != surface)) {
                        return false;
                    }
                    flat = surface;
                }
            }
            if (!flat) {
                return false;
            }
            rims.push_back(std::move(rim));
            rimsOn.push_back(*flat);
        }

        for (const Index t : theTriangles) {
            gone[t] = true;
        }
        for (std::size_t r = 0; r < rims.size(); ++r) {
            const std::vector<Index>& rim = rims[r];
            for (std::size_t i = 1; i + 1 < rim.size(); ++i) {
                added.push_back({rim[0], rim[i], rim[i + 1]});
                addedOn.push_back(rimsOn[r]);
            }
        }

        const bool outUp = closedLow || (!closedHigh && axis.cwiseAbs().maxCoeff() ==
                                                            axis.maxCoeff());
        const Vector3d out = outUp ? axis : Vector3d(-axis);
        const Vector3d opening = on + axis * (outUp ? high : low);
        const Vector3d helper =
            std::abs(out.x()) < 0.9 ? Vector3d(1.0, 0.0, 0.0) : Vector3d(0.0, 1.0, 0.0);
        const Vector3d x = out.cross(helper).normalized();
        const Vector3d y = out.cross(x);
        // On the plane's point nearest the origin, so that holes that open in one plane share a
        // sketch.
        PlannedHole hole;
        hole.plane = kernel::makePlane(point(out * opening.dot(out)), direction(out), direction(x));
        hole.at = {opening.dot(x), opening.dot(y)};
        hole.diameter = s.radius * 2.0;
        hole.depth = high - low;
        hole.tipAngle = tipAngle;
        taken.push_back(hole);
        return true;
    }

    Mesh& m_mesh;
    kernel::MeshSurfaces& m_found;
    const std::vector<kernel::Point>& m_refused;
    MeshFacts m_facts;
    std::vector<std::vector<Index>> m_touching;
};

} // namespace

Sharpened sharpen(const Mesh& mesh, const kernel::MeshSurfaces& found,
                  const std::vector<kernel::Point>& keep) {
    Sharpened out;
    out.mesh = mesh;
    out.found = found;
    if (!isClosed(mesh)) {
        return out;
    }
    double size = 0.0;
    for (const kernel::Point& p : mesh.vertices) {
        size = std::max({size, std::abs(p.x), std::abs(p.y), std::abs(p.z)});
    }
    const double close = std::max(size * 1e-8, 1e-9);

    std::vector<PlannedEdge> edges;
    for (int pass = 0; pass < mostPasses; ++pass) {
        std::vector<PlannedEdge> taken;
        {
            Pass step(out.mesh, out.found, keep);
            if (!step.edges(taken)) {
                break;
            }
        }
        tidy(out.mesh, out.found, close);
        edges.insert(edges.begin(), taken.begin(), taken.end());
    }
    std::vector<PlannedHole> holes;
    for (int pass = 0; pass < mostPasses; ++pass) {
        std::vector<PlannedHole> taken;
        {
            Pass step(out.mesh, out.found, keep);
            if (!step.holes(taken)) {
                break;
            }
        }
        tidy(out.mesh, out.found, close);
        holes.insert(holes.begin(), taken.begin(), taken.end());
    }
    if (!isClosed(out.mesh)) {
        // It did not stay one closed shape: then none of this happened.
        out.mesh = mesh;
        out.found = found;
        return out;
    }
    out.holes = std::move(holes);
    out.edges = std::move(edges);
    return out;
}

} // namespace lcad::recover
