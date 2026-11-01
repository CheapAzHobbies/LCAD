// SPDX-License-Identifier: GPL-3.0-or-later

#include "MeshFacts.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <tuple>
#include <utility>

namespace lcad::recover {

namespace {

using Eigen::Vector3d;
using kernel::MeshSurface;
using kernel::SurfaceKind;

constexpr double pi = std::numbers::pi;
// Faces that meet this smoothly are tangent, for a fillet: the cosine of the angle.
const double tangentCosine = std::cos(4.0 * pi / 180.0);
// A bevel is at most this wide, and a round at most this large, as a share of the part's size.
constexpr double widestChamfer = 0.15;
constexpr double largestRound = 0.25;

Vector3d unit(const Vector3d& v) {
    const double length = v.norm();
    return length > 1e-30 ? Vector3d(v / length) : Vector3d(0.0, 0.0, 1.0);
}

} // namespace

double offsetFrom(const MeshSurface& s, const Vector3d& p) {
    const Vector3d d = p - vec(s.origin);
    const Vector3d axis = vec(s.axis);
    switch (s.kind) {
    case SurfaceKind::Plane:
        return d.dot(axis);
    case SurfaceKind::Cylinder:
        return (d - axis * d.dot(axis)).norm() - s.radius;
    case SurfaceKind::Cone: {
        const double along = d.dot(axis);
        return (d - axis * along).norm() * std::cos(s.halfAngle) - along * std::sin(s.halfAngle);
    }
    case SurfaceKind::Sphere:
        return d.norm() - s.radius;
    case SurfaceKind::Torus: {
        const double along = d.dot(axis);
        return std::hypot((d - axis * along).norm() - s.radius, along) - s.tubeRadius;
    }
    }
    return 0.0;
}

Vector3d gradientOf(const MeshSurface& s, const Vector3d& p) {
    const Vector3d d = p - vec(s.origin);
    const Vector3d axis = vec(s.axis);
    switch (s.kind) {
    case SurfaceKind::Plane:
        return axis;
    case SurfaceKind::Cylinder:
        return unit(d - axis * d.dot(axis));
    case SurfaceKind::Cone:
        return unit(d - axis * d.dot(axis)) * std::cos(s.halfAngle) - axis * std::sin(s.halfAngle);
    case SurfaceKind::Sphere:
        return unit(d);
    case SurfaceKind::Torus: {
        const double along = d.dot(axis);
        const Vector3d out = d - axis * along;
        const double across = out.norm() - s.radius;
        const double length = std::max(std::hypot(across, along), 1e-30);
        return unit(out) * (across / length) + axis * (along / length);
    }
    }
    return axis;
}

Vector3d facing(const MeshSurface& s, const Vector3d& p) {
    return s.kind == SurfaceKind::Plane || s.outward ? gradientOf(s, p) : Vector3d(-gradientOf(s, p));
}

MeshFacts::MeshFacts(const kernel::Mesh& mesh, const kernel::MeshSurfaces& found)
    : m_mesh(mesh), m_found(found) {
    m_points.reserve(mesh.vertices.size());
    Vector3d low = Vector3d::Constant(std::numeric_limits<double>::max());
    Vector3d high = Vector3d::Constant(std::numeric_limits<double>::lowest());
    for (const kernel::Point& p : mesh.vertices) {
        m_points.push_back(vec(p));
        low = low.cwiseMin(m_points.back());
        high = high.cwiseMax(m_points.back());
    }
    m_size = (high - low).norm();
    for (const MeshSurface& s : found.surfaces) {
        m_area += s.area;
    }

    struct Side {
        Index low, high, triangle;
    };
    std::vector<Side> sides;
    sides.reserve(mesh.triangles.size() * 3);
    for (Index t = 0; t < mesh.triangles.size(); ++t) {
        const auto& c = mesh.triangles[t];
        for (std::size_t k = 0; k < 3; ++k) {
            const Index a = c[k];
            const Index b = c[(k + 1) % 3];
            sides.push_back({std::min(a, b), std::max(a, b), t});
        }
    }
    std::sort(sides.begin(), sides.end(), [](const Side& a, const Side& b) {
        return std::tie(a.low, a.high) < std::tie(b.low, b.high);
    });
    m_contacts.assign(found.surfaces.size(), {});
    for (std::size_t i = 0; i + 1 < sides.size(); ++i) {
        const Side& a = sides[i];
        const Side& b = sides[i + 1];
        if (a.low != b.low || a.high != b.high) {
            continue;
        }
        const Index s = found.surfaceOf[a.triangle];
        const Index t = found.surfaceOf[b.triangle];
        if (s == t) {
            continue;
        }
        const Vector3d p = m_points[a.low];
        const Vector3d q = m_points[a.high];
        const Vector3d middle = (p + q) / 2.0;
        const double length = (q - p).norm();
        const bool tangent = facing(found.surfaces[s], middle).dot(facing(found.surfaces[t], middle)) >
                             tangentCosine;
        for (const auto& [from, to] : {std::pair(s, t), std::pair(t, s)}) {
            Contact& contact = m_contacts[from][to];
            contact.length += length;
            contact.tangent += tangent ? length : 0.0;
        }
    }
}

double MeshFacts::sweepOf(Index number) const {
    const MeshSurface& s = m_found.surfaces[number];
    const Vector3d axis = vec(s.axis);
    const Vector3d helper =
        std::abs(axis.x()) < 0.9 ? Vector3d(1.0, 0.0, 0.0) : Vector3d(0.0, 1.0, 0.0);
    const Vector3d u = axis.cross(helper).normalized();
    const Vector3d v = axis.cross(u);
    std::vector<double> angles;
    for (const Index t : s.triangles) {
        for (const Index c : m_mesh.triangles[t]) {
            const Vector3d d = m_points[c] - vec(s.origin);
            angles.push_back(std::atan2(d.dot(v), d.dot(u)));
        }
    }
    if (angles.empty()) {
        return 0.0;
    }
    std::sort(angles.begin(), angles.end());
    double gap = angles.front() + 2.0 * pi - angles.back();
    for (std::size_t i = 1; i < angles.size(); ++i) {
        gap = std::max(gap, angles[i] - angles[i - 1]);
    }
    return 2.0 * pi - gap;
}

std::vector<Index> MeshFacts::tangentTo(Index s) const {
    std::vector<std::pair<double, Index>> found;
    for (const auto& [other, contact] : m_contacts[s]) {
        if (contact.tangent > contact.length * 0.5) {
            found.push_back({contact.tangent, other});
        }
    }
    std::sort(found.rbegin(), found.rend());
    std::vector<Index> out;
    out.reserve(found.size());
    for (const auto& [length, other] : found) {
        out.push_back(other);
    }
    return out;
}

bool MeshFacts::isRound(Index number) const {
    const MeshSurface& s = m_found.surfaces[number];
    if (s.facet || (s.kind != SurfaceKind::Cylinder && s.kind != SurfaceKind::Torus)) {
        return false;
    }
    // The end of a slot is half a circle, between sides that would never meet.
    if (s.kind == SurfaceKind::Cylinder && sweepOf(number) > pi * 0.95) {
        return false;
    }
    const double size = s.kind == SurfaceKind::Torus ? s.tubeRadius : s.radius;
    return size < m_size * largestRound && tangentTo(number).size() >= 2;
}

double MeshFacts::widthOf(Index number) const {
    double longest = 0.0;
    for (const auto& [other, contact] : m_contacts[number]) {
        longest = std::max(longest, contact.length);
    }
    return longest > 0.0 ? m_found.surfaces[number].area / longest : 0.0;
}

bool MeshFacts::isBevel(Index number) const {
    const MeshSurface& s = m_found.surfaces[number];
    if (s.facet || (s.kind != SurfaceKind::Plane && s.kind != SurfaceKind::Cone) ||
        m_contacts[number].size() < 2) {
        return false;
    }
    const double width = widthOf(number);
    return width > 0.0 && width < m_size * widestChamfer;
}

Vector3d MeshFacts::pointOn(Index number) const {
    const MeshSurface& s = m_found.surfaces[number];
    const auto middleOf = [this](Index t) {
        const auto& c = m_mesh.triangles[t];
        return Vector3d((m_points[c[0]] + m_points[c[1]] + m_points[c[2]]) / 3.0);
    };
    Vector3d sum = Vector3d::Zero();
    for (const Index t : s.triangles) {
        sum += middleOf(t);
    }
    const Vector3d middle = sum / std::max<double>(static_cast<double>(s.triangles.size()), 1.0);
    Vector3d best = middle;
    double nearest = std::numeric_limits<double>::max();
    for (const Index t : s.triangles) {
        const double distance = (middleOf(t) - middle).squaredNorm();
        if (distance < nearest) {
            nearest = distance;
            best = middleOf(t);
        }
    }
    return best;
}

std::optional<Vector3d> MeshFacts::meeting(const Vector3d& from, const std::vector<Index>& surfaces,
                                           double reach) const {
    const auto rows = static_cast<Eigen::Index>(surfaces.size());
    Eigen::MatrixXd gradients(rows, 3);
    Eigen::VectorXd offsets(rows);
    Vector3d p = from;
    const auto measure = [&] {
        for (Eigen::Index i = 0; i < rows; ++i) {
            const MeshSurface& s = m_found.surfaces[surfaces[static_cast<std::size_t>(i)]];
            offsets(i) = offsetFrom(s, p);
            gradients.row(i) = gradientOf(s, p).transpose();
        }
    };
    for (int step = 0; step < 32; ++step) {
        measure();
        const Vector3d change = gradients.completeOrthogonalDecomposition().solve(-offsets);
        p += change;
        if ((p - from).norm() > reach * 4.0) {
            return std::nullopt;
        }
        if (change.norm() < 1e-11) {
            break;
        }
    }
    measure();
    if (offsets.cwiseAbs().maxCoeff() > m_found.tolerance || (p - from).norm() > reach) {
        return std::nullopt;
    }
    return p;
}

} // namespace lcad::recover
