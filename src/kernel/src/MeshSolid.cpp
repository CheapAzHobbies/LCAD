// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/MeshSolid.h>

#include <lcad/kernel/Error.h>
#include <lcad/kernel/Stop.h>
#include <lcad/kernel/occt/Guard.h>
#include <lcad/kernel/occt/ShapeAccess.h>

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepLib.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box2d.hxx>
#include <ElCLib.hxx>
#include <ElSLib.hxx>
#include <GProp_GProps.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GeomAPI_IntSS.hxx>
#include <GeomAdaptor_Curve.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <Geom2dAPI_Interpolate.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <Geom2d_Curve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_Circle.hxx>
#include <Geom_ConicalSurface.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_Line.hxx>
#include <Geom_Plane.hxx>
#include <Geom_SphericalSurface.hxx>
#include <Geom_ToroidalSurface.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Precision.hxx>
#include <ShapeFix_Solid.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColStd_HArray1OfReal.hxx>
#include <TColgp_Array1OfPnt2d.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TColgp_HArray1OfPnt2d.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Cone.hxx>
#include <gp_Pln.hxx>

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <optional>

namespace lcad::kernel {

namespace {

using Index = std::uint32_t;

constexpr double pi = std::numbers::pi;
constexpr Index none = std::numeric_limits<Index>::max();

// A surface that closes up to within this angle is cut in two.
const double smallestGap = 40.0 * pi / 180.0;
// How many times over a part's surfaces are cut back to facets before all of it is.
constexpr int mostFacetRounds = 4;
// A piece of a sphere reaches at most this far from its middle, which keeps it off the poles.
const double sphereReach = 75.0 * pi / 180.0;

gp_Pnt pntOf(const Point& p) {
    return {p.x, p.y, p.z};
}

gp_Dir dirOf(const Vector& v) {
    return {v.x, v.y, v.z};
}

// Where each directed side of a triangle goes, and the side running the other way beside it.
// The sides of triangle t are 3t, 3t+1, and 3t+2.
class Sides {
public:
    explicit Sides(const Mesh& mesh) : m_mesh(mesh), m_twin(mesh.triangles.size() * 3, none) {
        struct Entry {
            Index low, high, side;
        };
        std::vector<Entry> entries;
        entries.reserve(m_twin.size());
        for (Index s = 0; s < m_twin.size(); ++s) {
            const Index a = from(s);
            const Index b = to(s);
            entries.push_back({std::min(a, b), std::max(a, b), s});
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return std::tie(a.low, a.high) < std::tie(b.low, b.high);
        });
        for (std::size_t i = 0; i < entries.size();) {
            std::size_t j = i + 1;
            while (j < entries.size() && entries[j].low == entries[i].low &&
                   entries[j].high == entries[i].high) {
                ++j;
            }
            // Two sides running opposite ways are a proper edge. Anything else leaves both open.
            if (j - i == 2 && from(entries[i].side) == to(entries[i + 1].side)) {
                m_twin[entries[i].side] = entries[i + 1].side;
                m_twin[entries[i + 1].side] = entries[i].side;
            }
            i = j;
        }
    }
    [[nodiscard]] Index triangle(Index side) const { return side / 3; }
    [[nodiscard]] Index from(Index side) const { return m_mesh.triangles[side / 3][side % 3]; }
    [[nodiscard]] Index to(Index side) const {
        return m_mesh.triangles[side / 3][(side % 3 + 1) % 3];
    }
    [[nodiscard]] Index next(Index side) const { return side / 3 * 3 + (side % 3 + 1) % 3; }
    [[nodiscard]] Index twin(Index side) const { return m_twin[side]; }
    [[nodiscard]] std::size_t size() const { return m_twin.size(); }

private:
    const Mesh& m_mesh;
    std::vector<Index> m_twin;
};

// A piece of the boundary between two surfaces.
struct Arc {
    Index surfaceA = none;
    Index surfaceB = none;
    /// The mesh's corners along it, in the direction it runs on surfaceA.
    std::vector<Index> corners;
    bool closed = false;
    TopoDS_Edge edge;
    /// Where each of the corners is on the edge's curve, rising along it.
    std::vector<double> parameters;
};

struct Step {
    Index arc;
    bool along;
};

Handle(Geom_Surface) geomSurface(const MeshSurface& s, const gp_Dir& reference) {
    const gp_Pnt origin = pntOf(s.origin);
    switch (s.kind) {
    case SurfaceKind::Plane:
        return new Geom_Plane(gp_Ax3(origin, dirOf(s.axis), reference));
    case SurfaceKind::Cylinder:
        return new Geom_CylindricalSurface(gp_Ax3(origin, dirOf(s.axis), reference), s.radius);
    case SurfaceKind::Cone: {
        // OCCT's cone is placed by a circle on it rather than by its apex.
        const gp_Dir axis = dirOf(s.axis);
        const double reach = 1.0;
        const gp_Pnt at = origin.Translated(gp_Vec(axis) * reach);
        return new Geom_ConicalSurface(gp_Ax3(at, axis, reference), s.halfAngle,
                                       reach * std::tan(s.halfAngle));
    }
    case SurfaceKind::Sphere:
        return new Geom_SphericalSurface(gp_Ax3(origin, dirOf(s.axis), reference), s.radius);
    case SurfaceKind::Torus:
        return new Geom_ToroidalSurface(gp_Ax3(origin, dirOf(s.axis), reference), s.radius,
                                        s.tubeRadius);
    }
    return nullptr;
}

class Builder {
public:
    Builder(const Mesh& mesh, const MeshSurfaces& found)
        : m_mesh(mesh), m_found(found), m_sides(mesh), m_tolerance(found.tolerance) {
        cutClosedSurfaces();
    }

    MeshSolids run() {
        MeshSolids out;
        std::vector<TopoDS_Shell> outer;
        std::vector<TopoDS_Shell> inner;
        for (const std::vector<Index>& part : parts()) {
            if (!closed(part)) {
                ++out.openParts;
                continue;
            }
            if (stopRequested()) {
                throw Error("make a solid from a mesh", "it was stopped");
            }
            TopoDS_Shell shell;
            try {
                // Once more with what would not come out as facets. Those can leave a neighbor
                // short of the corners it had, so it can take a few rounds.
                for (int round = 0; round < mostFacetRounds; ++round) {
                    m_refused.clear();
                    shell = exactShell(part);
                    if (m_refused.empty()) {
                        break;
                    }
                    for (const Index number : m_refused) {
                        toFacets(number);
                    }
                }
            } catch (const Standard_Failure&) {
                // Whatever went wrong in there, the facets below are still a solid.
                shell.Nullify();
            }
            // A solid that is not sound is worth less than one that is only faceted.
            if (!shell.IsNull()) {
                BRep_Builder builder;
                TopoDS_Solid trial;
                builder.MakeSolid(trial);
                builder.Add(trial, shell);
                if (!BRepCheck_Analyzer(trial).IsValid()) {
                    shell.Nullify();
                }
            }
            if (shell.IsNull()) {
                shell = facetShell(part);
                ++out.facetedParts;
            }
            if (shell.IsNull()) {
                ++out.failedParts;
                continue;
            }
            (signedVolume(part) >= 0.0 ? outer : inner).push_back(shell);
        }
        out.solids = assembled(outer, inner);
        return out;
    }

private:
    gp_Pnt middleOf(Index t) const {
        const auto& c = m_mesh.triangles[t];
        return gp_Pnt((pntOf(m_mesh.vertices[c[0]]).XYZ() + pntOf(m_mesh.vertices[c[1]]).XYZ() +
                       pntOf(m_mesh.vertices[c[2]]).XYZ()) /
                      3.0);
    }

    double triangleArea(Index t) const {
        const auto& c = m_mesh.triangles[t];
        return gp_Vec(pointAt(c[0]), pointAt(c[1])).Crossed(gp_Vec(pointAt(c[0]), pointAt(c[2]))).Magnitude() / 2.0;
    }

    // The widest gap in the angles at which `angle` finds the corners of a surface, and the
    // angle in the middle of it.
    template <typename Angle>
    std::pair<double, double> widestGap(const MeshSurface& s, Angle angle) const {
        std::vector<double> angles;
        angles.reserve(s.triangles.size() * 3);
        for (const Index t : s.triangles) {
            for (const Index c : m_mesh.triangles[t]) {
                angles.push_back(angle(pointAt(c)));
            }
        }
        std::sort(angles.begin(), angles.end());
        double widest = angles.front() + 2.0 * pi - angles.back();
        double at = angles.back() + widest / 2.0;
        for (std::size_t i = 1; i < angles.size(); ++i) {
            const double gap = angles[i] - angles[i - 1];
            if (gap > widest) {
                widest = gap;
                at = angles[i - 1] + gap / 2.0;
            }
        }
        return {widest, at};
    }

    // A face that goes all the way round a closed surface needs a seam, and one that covers a
    // pole of a sphere a degenerate edge. Rather than have either, such a surface is cut in two
    // along edges of the mesh, and once the solid stands the pieces are made one face again,
    // which OCCT does soundly.
    void cutClosedSurfaces() {
        m_surfaces = m_found.surfaces;
        m_surfaceOf = m_found.surfaceOf;
        for (Index s = 0; s < m_surfaces.size(); ++s) {
            for (int depth = 0; depth < 4; ++depth) {
                const std::optional<gp_Pln> cut = cutThrough(m_surfaces[s]);
                if (!cut || !cutApart(s, *cut)) {
                    break;
                }
            }
        }
    }

    // The plane to cut a surface along, if it has to be cut.
    std::optional<gp_Pln> cutThrough(const MeshSurface& s) const {
        if (s.facet || s.kind == SurfaceKind::Plane || s.triangles.empty()) {
            return std::nullopt;
        }
        const gp_Pnt origin = pntOf(s.origin);
        if (s.kind == SurfaceKind::Sphere) {
            gp_Vec mean(0.0, 0.0, 0.0);
            for (const Index t : s.triangles) {
                mean += gp_Vec(origin, middleOf(t)) * triangleArea(t);
            }
            if (mean.Magnitude() < 1e-12) {
                mean = gp_Vec(1.0, 0.0, 0.0);
            }
            mean.Normalize();
            double reach = 0.0;
            for (const Index t : s.triangles) {
                for (const Index c : m_mesh.triangles[t]) {
                    const gp_Vec d(origin, pointAt(c));
                    reach = std::max(reach, mean.Angle(d));
                }
            }
            if (reach < sphereReach) {
                return std::nullopt;
            }
            const gp_Vec helper =
                std::abs(mean.X()) < 0.9 ? gp_Vec(1.0, 0.0, 0.0) : gp_Vec(0.0, 1.0, 0.0);
            return gp_Pln(origin, gp_Dir(mean.Crossed(helper)));
        }
        const gp_Dir axis = dirOf(s.axis);
        const gp_Dir u = referenceFor(s);
        const gp_Dir v = axis.Crossed(u);
        const auto [gap, at] = widestGap(s, [&](const gp_Pnt& p) {
            const gp_Vec d(origin, p);
            return std::atan2(d.Dot(gp_Vec(v)), d.Dot(gp_Vec(u)));
        });
        if (gap >= smallestGap) {
            if (s.kind != SurfaceKind::Torus) {
                return std::nullopt;
            }
            // The tube of a torus closes up as well, though that is rare in a real model.
            const auto [tubeGap, tubeAt] = widestGap(s, [&](const gp_Pnt& p) {
                const gp_Vec d(origin, p);
                const double along = d.Dot(gp_Vec(axis));
                return std::atan2(along, (d - gp_Vec(axis) * along).Magnitude() - s.radius);
            });
            static_cast<void>(tubeAt);
            return tubeGap >= smallestGap ? std::nullopt
                                          : std::optional<gp_Pln>(gp_Pln(origin, axis));
        }
        const gp_Vec towards = gp_Vec(u) * std::cos(at) + gp_Vec(v) * std::sin(at);
        return gp_Pln(origin, gp_Dir(gp_Vec(axis).Crossed(towards)));
    }

    // Parts `s` by which side of `plane` its triangles lie, each connected piece a surface of its
    // own with the same geometry. False if that changes nothing.
    bool cutApart(Index s, const gp_Pln& plane) {
        const gp_Vec normal(plane.Axis().Direction());
        std::map<Index, char> side;
        for (const Index t : m_surfaces[s].triangles) {
            side[t] = gp_Vec(plane.Location(), middleOf(t)).Dot(normal) >= 0.0 ? 1 : 0;
        }
        std::map<Index, char> seen;
        std::vector<std::vector<Index>> pieces;
        for (const auto& [t, unused] : side) {
            if (seen.count(t) != 0) {
                continue;
            }
            std::vector<Index> piece;
            std::vector<Index> stack{t};
            seen[t] = 1;
            while (!stack.empty()) {
                const Index at = stack.back();
                stack.pop_back();
                piece.push_back(at);
                for (Index k = 0; k < 3; ++k) {
                    const Index twin = m_sides.twin(at * 3 + k);
                    if (twin == none) {
                        continue;
                    }
                    const Index other = m_sides.triangle(twin);
                    const auto found = side.find(other);
                    if (found != side.end() && found->second == side[at] &&
                        seen.count(other) == 0) {
                        seen[other] = 1;
                        stack.push_back(other);
                    }
                }
            }
            pieces.push_back(std::move(piece));
        }
        if (pieces.size() < 2) {
            return false;
        }
        const MeshSurface original = m_surfaces[s];
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            MeshSurface part = original;
            part.triangles = std::move(pieces[i]);
            part.area = 0.0;
            for (const Index t : part.triangles) {
                part.area += triangleArea(t);
            }
            const Index number = i == 0 ? s : static_cast<Index>(m_surfaces.size());
            for (const Index t : part.triangles) {
                m_surfaceOf[t] = number;
            }
            if (i == 0) {
                m_surfaces[s] = std::move(part);
            } else {
                m_surfaces.push_back(std::move(part));
            }
        }
        return true;
    }

    std::vector<std::vector<Index>> parts() const {
        std::vector<char> seen(m_mesh.triangles.size(), 0);
        std::vector<std::vector<Index>> out;
        for (Index t = 0; t < m_mesh.triangles.size(); ++t) {
            if (seen[t] != 0) {
                continue;
            }
            std::vector<Index> part;
            std::vector<Index> stack{t};
            seen[t] = 1;
            while (!stack.empty()) {
                const Index at = stack.back();
                stack.pop_back();
                part.push_back(at);
                for (Index k = 0; k < 3; ++k) {
                    const Index twin = m_sides.twin(at * 3 + k);
                    if (twin != none && seen[m_sides.triangle(twin)] == 0) {
                        seen[m_sides.triangle(twin)] = 1;
                        stack.push_back(m_sides.triangle(twin));
                    }
                }
            }
            out.push_back(std::move(part));
        }
        return out;
    }

    bool closed(const std::vector<Index>& part) const {
        return std::all_of(part.begin(), part.end(), [&](Index t) {
            return m_sides.twin(t * 3) != none && m_sides.twin(t * 3 + 1) != none &&
                   m_sides.twin(t * 3 + 2) != none;
        });
    }

    double signedVolume(const std::vector<Index>& part) const {
        double total = 0.0;
        for (const Index t : part) {
            const auto& c = m_mesh.triangles[t];
            const gp_XYZ a = pntOf(m_mesh.vertices[c[0]]).XYZ();
            const gp_XYZ b = pntOf(m_mesh.vertices[c[1]]).XYZ();
            const gp_XYZ d = pntOf(m_mesh.vertices[c[2]]).XYZ();
            total += a.Dot(b.Crossed(d));
        }
        return total / 6.0;
    }

    Index surfaceOfSide(Index side) const {
        return m_surfaceOf[m_sides.triangle(side)];
    }

    bool isBoundary(Index side) const {
        return surfaceOfSide(side) != surfaceOfSide(m_sides.twin(side));
    }

    // The next side around the same surface after `side`, turning about its end through the
    // surface's own triangles. This holds where a surface touches itself at a corner.
    Index nextBoundary(Index side) const {
        Index at = m_sides.next(side);
        while (!isBoundary(at)) {
            at = m_sides.next(m_sides.twin(at));
        }
        return at;
    }

    gp_Pnt pointAt(Index corner) const {
        return pntOf(m_mesh.vertices[corner]);
    }

    double offSurface(const MeshSurface& s, const gp_Pnt& p) const {
        const gp_Vec d(pntOf(s.origin), p);
        const gp_Vec axis(s.axis.x, s.axis.y, s.axis.z);
        switch (s.kind) {
        case SurfaceKind::Plane:
            return d.Dot(axis);
        case SurfaceKind::Cylinder:
            return (d - axis * d.Dot(axis)).Magnitude() - s.radius;
        case SurfaceKind::Cone: {
            const double along = d.Dot(axis);
            return (d - axis * along).Magnitude() * std::cos(s.halfAngle) -
                   along * std::sin(s.halfAngle);
        }
        case SurfaceKind::Sphere:
            return d.Magnitude() - s.radius;
        case SurfaceKind::Torus: {
            const double along = d.Dot(axis);
            return std::hypot((d - axis * along).Magnitude() - s.radius, along) - s.tubeRadius;
        }
        }
        return 0.0;
    }

    gp_Vec normalAt(const MeshSurface& s, const gp_Pnt& p) const {
        const gp_Vec d(pntOf(s.origin), p);
        const gp_Vec axis(s.axis.x, s.axis.y, s.axis.z);
        const auto unit = [](const gp_Vec& v) {
            return v.Magnitude() > 1e-30 ? v.Normalized() : gp_Vec(0.0, 0.0, 1.0);
        };
        switch (s.kind) {
        case SurfaceKind::Plane:
            return axis;
        case SurfaceKind::Cylinder:
            return unit(d - axis * d.Dot(axis));
        case SurfaceKind::Cone:
            return unit(d - axis * d.Dot(axis)) * std::cos(s.halfAngle) - axis * std::sin(s.halfAngle);
        case SurfaceKind::Sphere:
            return unit(d);
        case SurfaceKind::Torus: {
            const double along = d.Dot(axis);
            const gp_Vec out = d - axis * along;
            const double across = out.Magnitude() - s.radius;
            const double length = std::max(std::hypot(across, along), 1e-30);
            return unit(out) * (across / length) + axis * (along / length);
        }
        }
        return axis;
    }

    // Moves a point of the mesh onto all the real surfaces that meet there, as nearly as can be.
    gp_Pnt settled(gp_Pnt p, const std::vector<Index>& surfaces) const {
        std::vector<const MeshSurface*> real;
        for (const Index s : surfaces) {
            if (!m_surfaces[s].facet) {
                real.push_back(&m_surfaces[s]);
            }
        }
        if (real.empty()) {
            return p;
        }
        const gp_Pnt start = p;
        for (int step = 0; step < 8; ++step) {
            Eigen::MatrixX3d normals(static_cast<Eigen::Index>(real.size()), 3);
            Eigen::VectorXd offsets(static_cast<Eigen::Index>(real.size()));
            for (std::size_t i = 0; i < real.size(); ++i) {
                const gp_Vec n = normalAt(*real[i], p);
                normals.row(static_cast<Eigen::Index>(i)) << n.X(), n.Y(), n.Z();
                offsets(static_cast<Eigen::Index>(i)) = -offSurface(*real[i], p);
            }
            const Eigen::Vector3d change =
                normals.completeOrthogonalDecomposition().solve(offsets);
            if (!change.allFinite()) {
                return start;
            }
            p.Translate(gp_Vec(change.x(), change.y(), change.z()));
            if (change.norm() < 1e-11) {
                break;
            }
        }
        // Surfaces that hardly differ in direction there say little about where the point is.
        return p.Distance(start) > m_tolerance * 4.0 ? start : p;
    }

    std::vector<gp_Pnt> pointsOf(const Arc& arc) const {
        std::vector<gp_Pnt> points;
        points.reserve(arc.corners.size());
        for (const Index c : arc.corners) {
            points.push_back(pointAt(c));
        }
        return points;
    }

    // A straight line through points, if they lie on one.
    Handle(Geom_Curve) straight(const std::vector<gp_Pnt>& points, const Arc& arc,
                                double& first, double& last) const {
        const gp_Pnt& a = points.front();
        const gp_Pnt& b = points.back();
        const double length = a.Distance(b);
        if (length < 1e-9) {
            return nullptr;
        }
        const gp_Vec along = gp_Vec(a, b).Normalized();
        for (const gp_Pnt& p : points) {
            const gp_Vec d(a, p);
            if ((d - along * d.Dot(along)).Magnitude() > m_tolerance * 2.0) {
                return nullptr;
            }
        }
        // The line in which two planes meet is known better than the ends of a mesh edge are.
        const MeshSurface& s = m_surfaces[arc.surfaceA];
        const MeshSurface& t = m_surfaces[arc.surfaceB];
        gp_Pnt through = a;
        gp_Dir direction(along);
        if (s.kind == SurfaceKind::Plane && t.kind == SurfaceKind::Plane && !s.facet && !t.facet) {
            const gp_Vec cross = gp_Vec(dirOf(s.axis)).Crossed(gp_Vec(dirOf(t.axis)));
            if (cross.Magnitude() > 1e-3) {
                direction = gp_Dir(cross.Dot(along) >= 0.0 ? cross : -cross);
                through = settled(a, {arc.surfaceA, arc.surfaceB});
            }
        }
        first = gp_Vec(through, a).Dot(gp_Vec(direction));
        last = gp_Vec(through, b).Dot(gp_Vec(direction));
        return new Geom_Line(through, direction);
    }

    // A circle through points, if they lie on one, running the way they go.
    Handle(Geom_Curve) round(const std::vector<gp_Pnt>& points, bool isClosed, double& first,
                             double& last) const {
        const std::size_t count = points.size() - (isClosed ? 1 : 0);
        if (count < 3) {
            return nullptr;
        }
        Eigen::Vector3d middle = Eigen::Vector3d::Zero();
        for (std::size_t i = 0; i < count; ++i) {
            middle += Eigen::Vector3d(points[i].X(), points[i].Y(), points[i].Z());
        }
        middle /= static_cast<double>(count);
        Eigen::Matrix3d spread = Eigen::Matrix3d::Zero();
        for (std::size_t i = 0; i < count; ++i) {
            const Eigen::Vector3d d =
                Eigen::Vector3d(points[i].X(), points[i].Y(), points[i].Z()) - middle;
            spread += d * d.transpose();
        }
        const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(spread);
        if (solver.eigenvalues()(1) < 1e-16) {
            return nullptr;
        }
        Eigen::Vector3d normal = solver.eigenvectors().col(0);
        Eigen::Vector3d u = solver.eigenvectors().col(2);
        Eigen::Vector3d v = normal.cross(u);
        Eigen::MatrixX3d a(static_cast<Eigen::Index>(count), 3);
        Eigen::VectorXd b(static_cast<Eigen::Index>(count));
        std::vector<Eigen::Vector2d> flat(count);
        double outOfPlane = 0.0;
        for (std::size_t i = 0; i < count; ++i) {
            const Eigen::Vector3d d =
                Eigen::Vector3d(points[i].X(), points[i].Y(), points[i].Z()) - middle;
            flat[i] = {d.dot(u), d.dot(v)};
            outOfPlane = std::max(outOfPlane, std::abs(d.dot(normal)));
            a.row(static_cast<Eigen::Index>(i)) << 2.0 * flat[i].x(), 2.0 * flat[i].y(), 1.0;
            b(static_cast<Eigen::Index>(i)) = flat[i].squaredNorm();
        }
        if (outOfPlane > m_tolerance * 2.0) {
            return nullptr;
        }
        const Eigen::Vector3d s = a.colPivHouseholderQr().solve(b);
        Eigen::Vector2d center(s(0), s(1));
        const double squared = s(2) + center.squaredNorm();
        if (!std::isfinite(squared) || squared <= 0.0) {
            return nullptr;
        }
        double radius = std::sqrt(squared);
        for (int step = 0; step < 5; ++step) {
            Eigen::MatrixX3d jacobian(static_cast<Eigen::Index>(count), 3);
            Eigen::VectorXd residual(static_cast<Eigen::Index>(count));
            for (std::size_t i = 0; i < count; ++i) {
                const Eigen::Vector2d d = flat[i] - center;
                const double length = std::max(d.norm(), 1e-12);
                jacobian.row(static_cast<Eigen::Index>(i)) << -d.x() / length, -d.y() / length, -1.0;
                residual(static_cast<Eigen::Index>(i)) = length - radius;
            }
            const Eigen::Vector3d change = jacobian.colPivHouseholderQr().solve(-residual);
            center += change.head<2>();
            radius += change(2);
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (std::abs((flat[i] - center).norm() - radius) > m_tolerance * 2.0) {
                return nullptr;
            }
        }
        // Turning the way the points run, so that the parameter rises along them.
        double turned = 0.0;
        for (std::size_t i = 0; i + 1 < count; ++i) {
            const Eigen::Vector2d p = flat[i] - center;
            const Eigen::Vector2d q = flat[i + 1] - center;
            turned += p.x() * q.y() - p.y() * q.x();
        }
        if (turned < 0.0) {
            normal = -normal;
            v = -v;
        }
        const Eigen::Vector3d c3 = middle + u * center.x() + v * (turned < 0.0 ? -center.y() : center.y());
        const gp_Circ circle(gp_Ax2(gp_Pnt(c3.x(), c3.y(), c3.z()),
                                    gp_Dir(normal.x(), normal.y(), normal.z()),
                                    gp_Dir(u.x(), u.y(), u.z())),
                             radius);
        first = ElCLib::Parameter(circle, points.front());
        if (isClosed) {
            last = first + 2.0 * pi;
        } else {
            last = ElCLib::Parameter(circle, points.back());
            while (last <= first + 1e-9) {
                last += 2.0 * pi;
            }
        }
        return new Geom_Circle(circle);
    }

    // The curve in which the two surfaces meet, where OCCT can work it out: an ellipse, or the
    // curve between two cylinders.
    Handle(Geom_Curve) intersection(const std::vector<gp_Pnt>& points, const Arc& arc,
                                    double& first, double& last) {
        const MeshSurface& s = m_surfaces[arc.surfaceA];
        const MeshSurface& t = m_surfaces[arc.surfaceB];
        if (s.facet || t.facet) {
            return nullptr;
        }
        const Handle(Geom_Surface) a = geomSurface(s, referenceFor(s));
        const Handle(Geom_Surface) b = geomSurface(t, referenceFor(t));
        GeomAPI_IntSS meet(a, b, m_tolerance);
        if (!meet.IsDone()) {
            return nullptr;
        }
        const gp_Pnt& middle = points[points.size() / 2];
        Handle(Geom_Curve) best;
        double bestWorst = m_tolerance * 4.0;
        for (Standard_Integer i = 1; i <= meet.NbLines(); ++i) {
            const Handle(Geom_Curve) curve = meet.Line(i);
            double worst = 0.0;
            for (const gp_Pnt& p : {points.front(), middle, points.back()}) {
                GeomAPI_ProjectPointOnCurve project(p, curve);
                worst = std::max(worst, project.NbPoints() > 0 ? project.LowerDistance()
                                                               : std::numeric_limits<double>::max());
            }
            if (worst < bestWorst) {
                bestWorst = worst;
                best = curve;
            }
        }
        if (best.IsNull()) {
            return nullptr;
        }
        GeomAPI_ProjectPointOnCurve atStart(points.front(), best);
        GeomAPI_ProjectPointOnCurve atEnd(points.back(), best);
        GeomAPI_ProjectPointOnCurve atMiddle(middle, best);
        first = atStart.LowerDistanceParameter();
        last = atEnd.LowerDistanceParameter();
        double mid = atMiddle.LowerDistanceParameter();
        if (best->IsPeriodic()) {
            const double period = best->Period();
            const auto rising = [&](double from, double value) {
                while (value < from) {
                    value += period;
                }
                while (value >= from + period) {
                    value -= period;
                }
                return value;
            };
            mid = rising(first, mid);
            last = rising(first, last);
            if (arc.closed) {
                last = first + period;
            } else if (mid > last) {
                // It runs the other way round from the points.
                const Handle(Geom_Curve) reversed = best->Reversed();
                first = reversed->ReversedParameter(first);
                last = reversed->ReversedParameter(last);
                best = reversed;
                while (last <= first) {
                    last += period;
                }
            }
        } else if (first > last) {
            const Handle(Geom_Curve) reversed = best->Reversed();
            first = reversed->ReversedParameter(first);
            last = reversed->ReversedParameter(last);
            best = reversed;
        }
        return best;
    }

    // Through the corners, for a curve nothing simpler describes.
    Handle(Geom_Curve) through(const std::vector<gp_Pnt>& points, bool isClosed, double& first,
                               double& last) const {
        std::vector<gp_Pnt> kept;
        for (const gp_Pnt& p : points) {
            if (kept.empty() || kept.back().Distance(p) > 1e-7) {
                kept.push_back(p);
            }
        }
        if (isClosed && kept.size() > 1 && kept.back().Distance(kept.front()) < 1e-7) {
            kept.pop_back();
        }
        if (kept.size() < (isClosed ? 3u : 2u)) {
            return nullptr;
        }
        Handle(TColgp_HArray1OfPnt) array =
            new TColgp_HArray1OfPnt(1, static_cast<Standard_Integer>(kept.size()));
        for (std::size_t i = 0; i < kept.size(); ++i) {
            array->SetValue(static_cast<Standard_Integer>(i) + 1, kept[i]);
        }
        GeomAPI_Interpolate interpolate(array, isClosed ? Standard_True : Standard_False, 1e-7);
        interpolate.Perform();
        if (!interpolate.IsDone()) {
            return nullptr;
        }
        const Handle(Geom_BSplineCurve) curve = interpolate.Curve();
        first = curve->FirstParameter();
        last = curve->LastParameter();
        return Handle(Geom_Curve)(curve);
    }

    gp_Dir referenceFor(const MeshSurface& s) const {
        const gp_Dir axis = dirOf(s.axis);
        const gp_Dir helper = std::abs(axis.X()) < 0.9 ? gp_Dir(1.0, 0.0, 0.0) : gp_Dir(0.0, 1.0, 0.0);
        return gp_Dir(gp_Vec(axis).Crossed(gp_Vec(helper)));
    }

    bool makeEdge(Arc& arc, std::map<Index, TopoDS_Vertex>& vertices) {
        BRep_Builder builder;
        std::vector<gp_Pnt> points = pointsOf(arc);
        const auto vertexAt = [&](Index corner, const gp_Pnt& place) {
            const auto found = vertices.find(corner);
            if (found != vertices.end()) {
                return found->second;
            }
            TopoDS_Vertex vertex;
            builder.MakeVertex(vertex, place, m_tolerance);
            vertices.emplace(corner, vertex);
            return vertex;
        };

        double first = 0.0;
        double last = 0.0;
        Handle(Geom_Curve) curve;
        // How long the mesh says it is. A curve can run through all the corners and still be
        // wrong, by going the long way round.
        double length = 0.0;
        for (std::size_t i = 1; i < points.size(); ++i) {
            length += points[i - 1].Distance(points[i]);
        }
        const auto rightLength = [&]() {
            if (curve.IsNull()) {
                return false;
            }
            GeomAdaptor_Curve adaptor(curve, first, last);
            const double along = GCPnts_AbscissaPoint::Length(adaptor);
            return along >= length * 0.97 - m_tolerance && along <= length * 1.4 + m_tolerance * 4.0;
        };
        const auto attempt = [&](auto make) {
            if (rightLength()) {
                return;
            }
            try {
                curve = make();
            } catch (const Standard_Failure&) {
                curve.Nullify();
            }
        };
        if (!arc.closed) {
            attempt([&] { return straight(points, arc, first, last); });
        }
        attempt([&] { return round(points, arc.closed, first, last); });
        attempt([&] { return intersection(points, arc, first, last); });
        attempt([&] { return through(points, arc.closed, first, last); });
        if (curve.IsNull()) {
            return false;
        }
        const TopoDS_Vertex start = vertexAt(arc.corners.front(), points.front());
        const TopoDS_Vertex end =
            arc.closed ? start : vertexAt(arc.corners.back(), points.back());
        TopoDS_Edge edge;
        builder.MakeEdge(edge, curve, m_tolerance);
        builder.Add(edge, start.Oriented(TopAbs_FORWARD));
        builder.Add(edge, end.Oriented(TopAbs_REVERSED));
        builder.Range(edge, first, last);
        builder.UpdateVertex(start, std::max(m_tolerance, curve->Value(first).Distance(
                                                              BRep_Tool::Pnt(start)) * 1.1));
        builder.UpdateVertex(end, std::max(m_tolerance,
                                           curve->Value(last).Distance(BRep_Tool::Pnt(end)) * 1.1));
        arc.edge = edge;
        arc.parameters = parametersOn(curve, points, first, last);
        return true;
    }

    // Where points along a curve are on it, from `first` to `last`.
    static std::vector<double> parametersOn(const Handle(Geom_Curve)& curve,
                                            const std::vector<gp_Pnt>& points, double first,
                                            double last) {
        std::vector<double> parameters(points.size(), first);
        const Handle(Geom_Line) line = Handle(Geom_Line)::DownCast(curve);
        const Handle(Geom_Circle) circle = Handle(Geom_Circle)::DownCast(curve);
        for (std::size_t i = 1; i + 1 < points.size(); ++i) {
            double t = 0.0;
            if (!line.IsNull()) {
                t = ElCLib::Parameter(line->Lin(), points[i]);
            } else if (!circle.IsNull()) {
                t = ElCLib::Parameter(circle->Circ(), points[i]);
                while (t < parameters[i - 1]) {
                    t += 2.0 * pi;
                }
            } else {
                GeomAPI_ProjectPointOnCurve project(points[i], curve);
                t = project.NbPoints() > 0
                        ? project.LowerDistanceParameter()
                        : first + (last - first) * double(i) / double(points.size() - 1);
                // An ellipse, say: its projection knows nothing of which time round it is.
                if (curve->IsPeriodic()) {
                    t = ElCLib::InPeriod(t, parameters[i - 1], parameters[i - 1] + curve->Period());
                }
            }
            parameters[i] = t;
        }
        parameters.back() = last;
        // Rounding can put neighbors out of order, and a curve through them needs them in order.
        const double step = (last - first) * 1e-9;
        for (std::size_t i = 1; i + 1 < parameters.size(); ++i) {
            parameters[i] = std::clamp(parameters[i], parameters[i - 1] + step,
                                       last - step * double(parameters.size() - i));
        }
        return parameters;
    }

    // A point's parameters on a surface.
    static gp_Pnt2d onSurface(const MeshSurface& s, const Handle(Geom_Surface)& surface,
                              const gp_Pnt& p) {
        double u = 0.0;
        double v = 0.0;
        switch (s.kind) {
        case SurfaceKind::Plane:
            ElSLib::Parameters(Handle(Geom_Plane)::DownCast(surface)->Pln(), p, u, v);
            break;
        case SurfaceKind::Cylinder:
            ElSLib::Parameters(Handle(Geom_CylindricalSurface)::DownCast(surface)->Cylinder(), p, u,
                               v);
            break;
        case SurfaceKind::Cone:
            ElSLib::Parameters(Handle(Geom_ConicalSurface)::DownCast(surface)->Cone(), p, u, v);
            break;
        case SurfaceKind::Sphere:
            ElSLib::Parameters(Handle(Geom_SphericalSurface)::DownCast(surface)->Sphere(), p, u, v);
            break;
        case SurfaceKind::Torus:
            ElSLib::Parameters(Handle(Geom_ToroidalSurface)::DownCast(surface)->Torus(), p, u, v);
            break;
        }
        return {u, v};
    }

    // What an edge is on a face, in the surface's parameters: through the corners of the mesh,
    // which are on the surface, each at the parameter of the edge that it is at. This is worked
    // out, not left to the projection OCCT does, which on a closed surface can come out a whole
    // turn off, or go the long way round.
    Handle(Geom2d_Curve) onFace(const Arc& arc, const std::vector<gp_Pnt2d>& at) const {
        const auto count = static_cast<Standard_Integer>(at.size());
        if (count == 2) {
            // A straight line in the parameters, run through at the edge's own pace.
            TColgp_Array1OfPnt2d poles(1, 2);
            poles(1) = at[0];
            poles(2) = at[1];
            TColStd_Array1OfReal knots(1, 2);
            knots(1) = arc.parameters.front();
            knots(2) = arc.parameters.back();
            TColStd_Array1OfInteger multiplicities(1, 2);
            multiplicities.Init(2);
            return new Geom2d_BSplineCurve(poles, knots, multiplicities, 1);
        }
        Handle(TColgp_HArray1OfPnt2d) points = new TColgp_HArray1OfPnt2d(1, count);
        Handle(TColStd_HArray1OfReal) parameters = new TColStd_HArray1OfReal(1, count);
        for (Standard_Integer i = 1; i <= count; ++i) {
            points->SetValue(i, at[static_cast<std::size_t>(i - 1)]);
            parameters->SetValue(i, arc.parameters[static_cast<std::size_t>(i - 1)]);
        }
        Geom2dAPI_Interpolate interpolate(points, parameters, Standard_False, 1e-9);
        interpolate.Perform();
        if (interpolate.IsDone()) {
            const Handle(Geom2d_BSplineCurve) smooth = interpolate.Curve();
            // Corners close together on the curve and far apart on the surface can throw a smooth
            // curve wide of them.
            Bnd_Box2d around;
            for (const gp_Pnt2d& p : at) {
                around.Add(p);
            }
            around.Enlarge(0.25);
            bool stays = true;
            const double first = arc.parameters.front();
            const double last = arc.parameters.back();
            for (int i = 0; stays && i <= 64; ++i) {
                stays = !around.IsOut(smooth->Value(first + (last - first) * i / 64.0));
            }
            if (stays) {
                return Handle(Geom2d_Curve)(smooth);
            }
        }
        // Straight from corner to corner, then: never far off, and it can do no harm.
        TColgp_Array1OfPnt2d poles(1, count);
        TColStd_Array1OfReal knots(1, count);
        TColStd_Array1OfInteger multiplicities(1, count);
        for (Standard_Integer i = 1; i <= count; ++i) {
            poles(i) = at[static_cast<std::size_t>(i - 1)];
            knots(i) = arc.parameters[static_cast<std::size_t>(i - 1)];
            multiplicities(i) = i == 1 || i == count ? 2 : 1;
        }
        return new Geom2d_BSplineCurve(poles, knots, multiplicities, 1);
    }

    // The direction around the axis in which the seam of a surface is out of the way of the
    // face: the middle of the widest gap in the angles the face takes up.
    gp_Dir seamAwayFrom(const MeshSurface& s) const {
        const gp_Dir axis = dirOf(s.axis);
        const gp_Dir u = referenceFor(s);
        const gp_Dir v = axis.Crossed(u);
        const auto [gap, at] = widestGap(s, [&](const gp_Pnt& p) {
            const gp_Vec d(pntOf(s.origin), p);
            return std::atan2(d.Dot(gp_Vec(v)), d.Dot(gp_Vec(u)));
        });
        static_cast<void>(gap);
        return gp_Dir(gp_Vec(u) * std::cos(at) + gp_Vec(v) * std::sin(at));
    }

    // A sphere has poles as well as a seam: they are put where the face is not, around its side.
    MeshSurface placedSphere(MeshSurface s, const std::vector<Index>& triangles) const {
        gp_Vec around(0.0, 0.0, 0.0);
        for (const Index t : triangles) {
            for (const Index c : m_mesh.triangles[t]) {
                around += gp_Vec(pntOf(s.origin), pointAt(c));
            }
        }
        if (around.Magnitude() < 1e-9) {
            around = gp_Vec(1.0, 0.0, 0.0);
        }
        around.Normalize();
        const gp_Vec helper = std::abs(around.X()) < 0.9 ? gp_Vec(1.0, 0.0, 0.0) : gp_Vec(0.0, 1.0, 0.0);
        const gp_Vec axis = around.Crossed(helper).Normalized();
        s.axis = {axis.X(), axis.Y(), axis.Z()};
        return s;
    }

    TopoDS_Face makeFace(Index number, const std::vector<std::vector<Step>>& loops,
                         const std::vector<Arc>& arcs) const {
        MeshSurface s = m_surfaces[number];
        if (s.kind == SurfaceKind::Sphere) {
            s = placedSphere(s, s.triangles);
        }
        const gp_Dir reference = s.kind == SurfaceKind::Plane ? referenceFor(s) : seamAwayFrom(s);
        const Handle(Geom_Surface) surface = geomSurface(s, reference);
        // A surface's own outside is away from the axis. Where the mesh faces the other way, as
        // in a hole, the face runs the other way round on it and is turned over at the end.
        const bool turned = s.kind != SurfaceKind::Plane && !s.outward;
        BRep_Builder builder;
        if (loops.empty()) {
            // A whole sphere or torus: it has no edges but those of the surface itself.
            BRepBuilderAPI_MakeFace whole(surface, Precision::Confusion());
            return whole.IsDone() ? (turned ? TopoDS::Face(whole.Face().Reversed()) : whole.Face())
                                  : TopoDS_Face();
        }
        const auto wires = [&](TopoDS_Face& face) {
            for (const std::vector<Step>& loop : loops) {
                TopoDS_Wire wire;
                builder.MakeWire(wire);
                const auto add = [&](const Step& step, bool backward) {
                    const TopoDS_Edge& edge = arcs[step.arc].edge;
                    builder.Add(wire, edge.Oriented(step.along != backward ? TopAbs_FORWARD
                                                                           : TopAbs_REVERSED));
                };
                if (turned) {
                    for (auto step = loop.rbegin(); step != loop.rend(); ++step) {
                        add(*step, true);
                    }
                } else {
                    for (const Step& step : loop) {
                        add(step, false);
                    }
                }
                wire.Closed(Standard_True);
                builder.Add(face, wire);
            }
        };
        TopoDS_Face face;
        builder.MakeFace(face, surface, m_tolerance);
        wires(face);
        // A plane works out the curves on it by itself, exactly.
        if (s.kind != SurfaceKind::Plane) {
            // Around the middle of the face, for it not to cross where the parameters wrap: the
            // seam is where the face is not, but the tube of a torus has a seam of its own.
            double sinU = 0.0;
            double cosU = 0.0;
            double sinV = 0.0;
            double cosV = 0.0;
            for (const Index t : s.triangles) {
                const gp_Pnt2d at = onSurface(s, surface, middleOf(t));
                sinU += std::sin(at.X());
                cosU += std::cos(at.X());
                sinV += std::sin(at.Y());
                cosV += std::cos(at.Y());
            }
            const double middleU = std::atan2(sinU, cosU);
            const double middleV = std::atan2(sinV, cosV);
            const bool wrapsV = s.kind == SurfaceKind::Torus;
            const auto around = [](double value, double middle) {
                return value + 2.0 * pi * std::round((middle - value) / (2.0 * pi));
            };
            for (const std::vector<Step>& loop : loops) {
                for (const Step& step : loop) {
                    const Arc& arc = arcs[step.arc];
                    std::vector<gp_Pnt2d> at;
                    at.reserve(arc.corners.size());
                    for (const Index corner : arc.corners) {
                        const gp_Pnt2d uv = onSurface(s, surface, pointAt(corner));
                        at.emplace_back(around(uv.X(), middleU),
                                        wrapsV ? around(uv.Y(), middleV) : uv.Y());
                    }
                    const Handle(Geom2d_Curve) curve = onFace(arc, at);
                    if (curve.IsNull()) {
                        return {};
                    }
                    builder.UpdateEdge(arc.edge, curve, face, m_tolerance);
                }
            }
        }
        return turned ? TopoDS::Face(face.Reversed()) : face;
    }

    TopoDS_Shell exactShell(const std::vector<Index>& part) {
        // The boundaries between surfaces, and the corners where they branch.
        std::map<Index, int> degree;
        std::vector<Index> boundary;
        for (const Index t : part) {
            for (Index k = 0; k < 3; ++k) {
                const Index side = t * 3 + k;
                if (isBoundary(side)) {
                    boundary.push_back(side);
                    ++degree[m_sides.from(side)];
                }
            }
        }
        const auto facetSide = [&](Index side) {
            return m_surfaces[surfaceOfSide(side)].facet ||
                   m_surfaces[surfaceOfSide(m_sides.twin(side))].facet;
        };
        const auto isCorner = [&](Index corner) {
            const auto found = degree.find(corner);
            return found != degree.end() && found->second != 2;
        };
        // Along a facet the boundary is cut at every corner of the mesh: a facet has straight sides.
        std::vector<char> breakAt(m_mesh.vertices.size(), 0);
        for (const Index side : boundary) {
            if (facetSide(side)) {
                breakAt[m_sides.from(side)] = 1;
                breakAt[m_sides.to(side)] = 1;
            }
        }
        const auto breaks = [&](Index corner) { return isCorner(corner) || breakAt[corner] != 0; };

        std::vector<Arc> arcs;
        std::map<Index, Step> stepOf;
        const auto trace = [&](Index start) {
            const auto number = static_cast<Index>(arcs.size());
            Arc arc;
            arc.surfaceA = surfaceOfSide(start);
            arc.surfaceB = surfaceOfSide(m_sides.twin(start));
            arc.corners.push_back(m_sides.from(start));
            Index at = start;
            while (true) {
                stepOf[at] = {number, true};
                stepOf[m_sides.twin(at)] = {number, false};
                arc.corners.push_back(m_sides.to(at));
                if (breaks(m_sides.to(at))) {
                    break;
                }
                at = nextBoundary(at);
                if (at == start) {
                    arc.closed = true;
                    break;
                }
            }
            arcs.push_back(std::move(arc));
        };
        for (const Index side : boundary) {
            if (stepOf.count(side) == 0 && breaks(m_sides.from(side))) {
                trace(side);
            }
        }
        for (const Index side : boundary) {
            if (stepOf.count(side) == 0) {
                trace(side);
            }
        }

        std::map<Index, TopoDS_Vertex> vertices;
        for (Arc& arc : arcs) {
            if (!makeEdge(arc, vertices)) {
                return {};
            }
        }

        std::map<Index, std::vector<std::vector<Step>>> loops;
        std::map<Index, char> visited;
        std::map<Index, char> surfacesHere;
        for (const Index t : part) {
            surfacesHere[m_surfaceOf[t]] = 1;
        }
        for (const Index side : boundary) {
            if (visited.count(side) != 0 || !(breaks(m_sides.from(side)) || stepOf[side].arc == none)) {
                continue;
            }
            std::vector<Step> loop;
            Index at = side;
            do {
                visited[at] = 1;
                const Step step = stepOf[at];
                if (loop.empty() || loop.back().arc != step.arc || loop.back().along != step.along ||
                    breaks(m_sides.from(at))) {
                    loop.push_back(step);
                }
                at = nextBoundary(at);
            } while (at != side);
            loops[surfaceOfSide(side)].push_back(std::move(loop));
        }
        // What is left has no corners at all: a closed curve of its own.
        for (const Index side : boundary) {
            if (visited.count(side) != 0) {
                continue;
            }
            Index at = side;
            do {
                visited[at] = 1;
                at = nextBoundary(at);
            } while (at != side);
            loops[surfaceOfSide(side)].push_back({stepOf[side]});
        }

        TopoDS_Shell result;
        BRep_Builder builder;
        builder.MakeShell(result);
        for (const auto& [number, unused] : surfacesHere) {
            const auto found = loops.find(number);
            static const std::vector<std::vector<Step>> noLoops;
            const TopoDS_Face face =
                makeFace(number, found == loops.end() ? noLoops : found->second, arcs);
            if (face.IsNull() || !keepsItsSide(face, m_surfaces[number])) {
                m_refused.push_back(number);
            } else {
                builder.Add(result, face);
            }
        }
        if (!m_refused.empty()) {
            return {};
        }
        result.Closed(Standard_True);
        // An edge against a facet follows the facet, a little off the curved surface beside it.
        // The edge's tolerance says how far.
        BRepLib::SameParameter(result, m_tolerance, Standard_True);
        return BRep_Tool::IsClosed(result) ? result : TopoDS_Shell();
    }

    // A surface that would not come out is cut back to the flat triangles of the mesh, which
    // always do. The rest of the part stays as exact as it was.
    void toFacets(Index number) {
        const std::vector<Index> triangles = m_surfaces[number].triangles;
        MeshSurface facet;
        facet.kind = SurfaceKind::Plane;
        facet.facet = true;
        for (std::size_t i = 0; i < triangles.size(); ++i) {
            const Index t = triangles[i];
            const auto& c = m_mesh.triangles[t];
            const gp_Vec cross = gp_Vec(pointAt(c[0]), pointAt(c[1]))
                                     .Crossed(gp_Vec(pointAt(c[0]), pointAt(c[2])));
            if (cross.Magnitude() > 0.0) {
                const gp_Dir normal(cross);
                facet.axis = {normal.X(), normal.Y(), normal.Z()};
            }
            facet.origin = m_mesh.vertices[c[0]];
            facet.triangles = {t};
            facet.area = triangleArea(t);
            if (i == 0) {
                m_surfaces[number] = facet;
            } else {
                m_surfaceOf[t] = static_cast<Index>(m_surfaces.size());
                m_surfaces.push_back(facet);
            }
        }
    }

    // A face on a closed surface can come out as the rest of the surface, the outline being the
    // same either way. Such a face is sound, so no check of its geometry would notice.
    bool keepsItsSide(const TopoDS_Face& face, const MeshSurface& surface) const {
        GProp_GProps properties;
        BRepGProp::SurfaceProperties(face, properties);
        const double area = properties.Mass();
        return std::abs(area - surface.area) <= surface.area * 0.25 + m_tolerance * m_tolerance * 10.0;
    }

    TopoDS_Shell facetShell(const std::vector<Index>& part) const {
        BRep_Builder builder;
        std::map<Index, TopoDS_Vertex> vertices;
        std::map<std::pair<Index, Index>, TopoDS_Edge> edges;
        const auto vertexAt = [&](Index corner) {
            const auto found = vertices.find(corner);
            if (found != vertices.end()) {
                return found->second;
            }
            TopoDS_Vertex vertex;
            builder.MakeVertex(vertex, pointAt(corner), m_tolerance);
            vertices.emplace(corner, vertex);
            return vertex;
        };
        TopoDS_Shell shell;
        builder.MakeShell(shell);
        for (const Index t : part) {
            const auto& c = m_mesh.triangles[t];
            const gp_Vec cross = gp_Vec(pointAt(c[0]), pointAt(c[1]))
                                     .Crossed(gp_Vec(pointAt(c[0]), pointAt(c[2])));
            if (cross.Magnitude() < 1e-14) {
                return {};
            }
            const Handle(Geom_Plane) plane = new Geom_Plane(pointAt(c[0]), gp_Dir(cross));
            TopoDS_Wire wire;
            builder.MakeWire(wire);
            for (std::size_t k = 0; k < 3; ++k) {
                const Index a = c[k];
                const Index b = c[(k + 1) % 3];
                const std::pair<Index, Index> key{std::min(a, b), std::max(a, b)};
                auto found = edges.find(key);
                if (found == edges.end()) {
                    const gp_Pnt p = pointAt(key.first);
                    const gp_Pnt q = pointAt(key.second);
                    if (p.Distance(q) < 1e-9) {
                        return {};
                    }
                    TopoDS_Edge edge;
                    builder.MakeEdge(edge, new Geom_Line(p, gp_Dir(gp_Vec(p, q))), m_tolerance);
                    builder.Add(edge, vertexAt(key.first).Oriented(TopAbs_FORWARD));
                    builder.Add(edge, vertexAt(key.second).Oriented(TopAbs_REVERSED));
                    builder.Range(edge, 0.0, p.Distance(q));
                    found = edges.emplace(key, edge).first;
                }
                builder.Add(wire, found->second.Oriented(a == key.first ? TopAbs_FORWARD
                                                                        : TopAbs_REVERSED));
            }
            TopoDS_Face face;
            builder.MakeFace(face, plane, m_tolerance);
            builder.Add(face, wire);
            builder.Add(shell, face);
        }
        shell.Closed(Standard_True);
        return shell;
    }

    std::vector<Shape> assembled(const std::vector<TopoDS_Shell>& outer,
                                 const std::vector<TopoDS_Shell>& inner) const {
        BRep_Builder builder;
        std::vector<TopoDS_Solid> solids;
        for (const TopoDS_Shell& shell : outer) {
            TopoDS_Solid solid;
            builder.MakeSolid(solid);
            builder.Add(solid, shell);
            solids.push_back(solid);
        }
        // A closed part facing inward is a hollow in whichever solid is around it.
        for (const TopoDS_Shell& hollow : inner) {
            TopExp_Explorer vertices(hollow, TopAbs_VERTEX);
            if (!vertices.More()) {
                continue;
            }
            const gp_Pnt at = BRep_Tool::Pnt(TopoDS::Vertex(vertices.Current()));
            for (TopoDS_Solid& solid : solids) {
                BRepClass3d_SolidClassifier classifier(solid, at, m_tolerance);
                if (classifier.State() == TopAbs_IN) {
                    builder.Add(solid, hollow);
                    break;
                }
            }
        }
        std::vector<Shape> out;
        for (const TopoDS_Solid& solid : solids) {
            ShapeFix_Solid fix(solid);
            fix.Perform();
            // The pieces of surfaces that were cut stay apart: joining them up takes far longer
            // than all the rest, and fails on some. The viewport shows no line between them.
            out.push_back(occt::wrap(fix.Solid()));
        }
        return out;
    }

    const Mesh& m_mesh;
    const MeshSurfaces& m_found;
    Sides m_sides;
    double m_tolerance;
    std::vector<MeshSurface> m_surfaces;
    std::vector<Index> m_surfaceOf;
    // The surfaces that exactShell() could not make faces of.
    std::vector<Index> m_refused;
};

} // namespace

MeshSolids solidsFromMesh(const Mesh& mesh, const MeshSurfaces& found) {
    return occt::guarded("make a solid from a mesh", [&] { return Builder(mesh, found).run(); });
}

double meshVolume(const Mesh& mesh) {
    double total = 0.0;
    for (const auto& c : mesh.triangles) {
        const Point& a = mesh.vertices[c[0]];
        const Point& b = mesh.vertices[c[1]];
        const Point& d = mesh.vertices[c[2]];
        total += a.x * (b.y * d.z - b.z * d.y) - a.y * (b.x * d.z - b.z * d.x) +
                 a.z * (b.x * d.y - b.y * d.x);
    }
    return total / 6.0;
}

} // namespace lcad::kernel
