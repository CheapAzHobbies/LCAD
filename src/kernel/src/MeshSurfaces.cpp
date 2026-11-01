// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/MeshSurfaces.h>

#include <lcad/kernel/Error.h>
#include <lcad/kernel/Stop.h>

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <numbers>
#include <numeric>
#include <optional>

namespace lcad::kernel {

namespace {

using Eigen::Vector3d;
using Index = std::uint32_t;

constexpr double pi = std::numbers::pi;
constexpr Index none = std::numeric_limits<Index>::max();

// Neighbouring flat pieces this close in angle may be pieces of one curved surface. The coarsest
// meshes that CAD programs write bend by up to about this much from one facet to the next.
const double smoothAngle = 40.0 * pi / 180.0;
// Triangles of one flat patch face the same way to within this.
const double flatAngle = 1.0 * pi / 180.0;
// A facet of a curved surface leans this far at most from the surface at its middle.
const double leanAngle = 45.0 * pi / 180.0;
// A corner may be this many tolerances from a surface while the surface is being grown; a fit to
// a few facets is less exact than the final one to all of them.
constexpr double growSlack = 1.5;
// A surface this many times larger than the mesh is a flat piece seen as a curved one.
constexpr double largestRadius = 20.0;
// What counts as a neighbour of comparable size when a surface is started: a large flat
// neighbour is the face that the curve ends on, not part of it.
constexpr double seedSizeRatio = 8.0;

Vector3d vec(const Point& p) {
    return {p.x, p.y, p.z};
}

Point point(const Vector3d& v) {
    return {v.x(), v.y(), v.z()};
}

Vector direction(const Vector3d& v) {
    return {v.x(), v.y(), v.z()};
}

// A flat piece: triangles that are connected and lie in one plane.
struct Patch {
    std::vector<Index> triangles;
    std::vector<Index> corners;
    double area = 0.0;
    Vector3d normal = Vector3d::Zero();
    Vector3d middle = Vector3d::Zero();
    struct Neighbour {
        Index patch;
        double angle;
    };
    std::vector<Neighbour> neighbours;
};

// One of the surfaces being looked for, as distances from it and directions off it.
class Surface {
public:
    virtual ~Surface() = default;
    [[nodiscard]] virtual double distance(const Vector3d& p) const = 0;
    [[nodiscard]] virtual Vector3d normal(const Vector3d& p) const = 0;
    [[nodiscard]] virtual SurfaceKind kind() const = 0;
    virtual void describe(MeshSurface& out) const = 0;
    /// The surface a small step `step` away. Empty once there are no more parameters.
    [[nodiscard]] virtual std::unique_ptr<Surface> stepped(int parameter, double step) const = 0;
    [[nodiscard]] virtual int parameters() const = 0;
};

std::pair<Vector3d, Vector3d> basisAround(const Vector3d& axis) {
    const Vector3d helper =
        std::abs(axis.x()) < 0.9 ? Vector3d(1.0, 0.0, 0.0) : Vector3d(0.0, 1.0, 0.0);
    const Vector3d u = axis.cross(helper).normalized();
    return {u, axis.cross(u)};
}

Vector3d radial(const Vector3d& fromAxis, const Vector3d& axis) {
    return fromAxis - axis * fromAxis.dot(axis);
}

class Cylinder final : public Surface {
public:
    Cylinder(const Vector3d& axis, const Vector3d& center, double radius)
        : m_axis(axis), m_center(center), m_radius(radius) {}
    double distance(const Vector3d& p) const override {
        return radial(p - m_center, m_axis).norm() - m_radius;
    }
    Vector3d normal(const Vector3d& p) const override {
        return radial(p - m_center, m_axis).normalized();
    }
    SurfaceKind kind() const override { return SurfaceKind::Cylinder; }
    void describe(MeshSurface& out) const override {
        out.origin = point(m_center);
        out.axis = direction(m_axis);
        out.radius = m_radius;
    }
    int parameters() const override { return 5; }
    std::unique_ptr<Surface> stepped(int parameter, double step) const override {
        const auto [u, v] = basisAround(m_axis);
        Vector3d axis = m_axis;
        Vector3d center = m_center;
        double radius = m_radius;
        switch (parameter) {
        case 0: axis += u * step; break;
        case 1: axis += v * step; break;
        case 2: center += u * step; break;
        case 3: center += v * step; break;
        default: radius += step; break;
        }
        return std::make_unique<Cylinder>(axis.normalized(), center, radius);
    }

private:
    Vector3d m_axis;
    Vector3d m_center;
    double m_radius;
};

class Cone final : public Surface {
public:
    Cone(const Vector3d& axis, const Vector3d& apex, double angle)
        : m_axis(axis), m_apex(apex), m_angle(angle) {}
    double distance(const Vector3d& p) const override {
        const Vector3d d = p - m_apex;
        const double along = d.dot(m_axis);
        return radial(d, m_axis).norm() * std::cos(m_angle) - along * std::sin(m_angle);
    }
    Vector3d normal(const Vector3d& p) const override {
        return radial(p - m_apex, m_axis).normalized() * std::cos(m_angle) -
               m_axis * std::sin(m_angle);
    }
    SurfaceKind kind() const override { return SurfaceKind::Cone; }
    void describe(MeshSurface& out) const override {
        out.origin = point(m_apex);
        out.axis = direction(m_axis);
        out.halfAngle = m_angle;
    }
    int parameters() const override { return 6; }
    std::unique_ptr<Surface> stepped(int parameter, double step) const override {
        const auto [u, v] = basisAround(m_axis);
        Vector3d axis = m_axis;
        Vector3d apex = m_apex;
        double angle = m_angle;
        switch (parameter) {
        case 0: axis += u * step; break;
        case 1: axis += v * step; break;
        case 2: apex += u * step; break;
        case 3: apex += v * step; break;
        case 4: apex += m_axis * step; break;
        default: angle += step; break;
        }
        return std::make_unique<Cone>(axis.normalized(), apex, angle);
    }

private:
    Vector3d m_axis;
    Vector3d m_apex;
    double m_angle;
};

class Sphere final : public Surface {
public:
    Sphere(const Vector3d& center, double radius) : m_center(center), m_radius(radius) {}
    double distance(const Vector3d& p) const override { return (p - m_center).norm() - m_radius; }
    Vector3d normal(const Vector3d& p) const override { return (p - m_center).normalized(); }
    SurfaceKind kind() const override { return SurfaceKind::Sphere; }
    void describe(MeshSurface& out) const override {
        out.origin = point(m_center);
        out.radius = m_radius;
    }
    int parameters() const override { return 4; }
    std::unique_ptr<Surface> stepped(int parameter, double step) const override {
        Vector3d center = m_center;
        double radius = m_radius;
        if (parameter < 3) {
            center[parameter] += step;
        } else {
            radius += step;
        }
        return std::make_unique<Sphere>(center, radius);
    }

private:
    Vector3d m_center;
    double m_radius;
};

class Torus final : public Surface {
public:
    Torus(const Vector3d& axis, const Vector3d& center, double radius, double tube)
        : m_axis(axis), m_center(center), m_radius(radius), m_tube(tube) {}
    double distance(const Vector3d& p) const override {
        const Vector3d d = p - m_center;
        const double along = d.dot(m_axis);
        return std::hypot(radial(d, m_axis).norm() - m_radius, along) - m_tube;
    }
    Vector3d normal(const Vector3d& p) const override {
        const Vector3d d = p - m_center;
        const double along = d.dot(m_axis);
        const Vector3d out = radial(d, m_axis);
        const double across = out.norm() - m_radius;
        const double length = std::max(std::hypot(across, along), 1e-30);
        return out.normalized() * (across / length) + m_axis * (along / length);
    }
    SurfaceKind kind() const override { return SurfaceKind::Torus; }
    void describe(MeshSurface& out) const override {
        out.origin = point(m_center);
        out.axis = direction(m_axis);
        out.radius = m_radius;
        out.tubeRadius = m_tube;
    }
    int parameters() const override { return 7; }
    std::unique_ptr<Surface> stepped(int parameter, double step) const override {
        const auto [u, v] = basisAround(m_axis);
        Vector3d axis = m_axis;
        Vector3d center = m_center;
        double radius = m_radius;
        double tube = m_tube;
        switch (parameter) {
        case 0: axis += u * step; break;
        case 1: axis += v * step; break;
        case 2: center += u * step; break;
        case 3: center += v * step; break;
        case 4: center += m_axis * step; break;
        case 5: radius += step; break;
        default: tube += step; break;
        }
        return std::make_unique<Torus>(axis.normalized(), center, radius, tube);
    }

private:
    Vector3d m_axis;
    Vector3d m_center;
    double m_radius;
    double m_tube;
};

// The circle nearest to points in a plane: first the algebraic fit, which needs no start, then
// steps along the true distances. A short arc is what a fillet's facets give, and the algebraic
// fit alone is biased there.
std::optional<std::pair<Eigen::Vector2d, double>> circleThrough(const Eigen::MatrixX2d& points) {
    const auto n = points.rows();
    if (n < 3) {
        return std::nullopt;
    }
    Eigen::MatrixX3d a(n, 3);
    Eigen::VectorXd b(n);
    for (Eigen::Index i = 0; i < n; ++i) {
        a.row(i) << 2.0 * points(i, 0), 2.0 * points(i, 1), 1.0;
        b(i) = points.row(i).squaredNorm();
    }
    const Eigen::Vector3d s = a.colPivHouseholderQr().solve(b);
    Eigen::Vector2d center(s(0), s(1));
    const double squared = s(2) + center.squaredNorm();
    if (!std::isfinite(squared) || squared <= 0.0) {
        return std::nullopt;
    }
    double radius = std::sqrt(squared);
    for (int step = 0; step < 6; ++step) {
        Eigen::MatrixX3d jacobian(n, 3);
        Eigen::VectorXd residual(n);
        for (Eigen::Index i = 0; i < n; ++i) {
            const Eigen::Vector2d d = points.row(i).transpose() - center;
            const double length = std::max(d.norm(), 1e-12);
            jacobian.row(i) << -d.x() / length, -d.y() / length, -1.0;
            residual(i) = length - radius;
        }
        const Eigen::Vector3d change = jacobian.colPivHouseholderQr().solve(-residual);
        center += change.head<2>();
        radius += change(2);
    }
    if (!center.allFinite() || !std::isfinite(radius) || radius <= 0.0) {
        return std::nullopt;
    }
    return std::make_pair(center, radius);
}

// Least squares steps of all of a surface's parameters against the distances of `points`. The
// derivatives are taken numerically: there are at most seven parameters and this runs once per
// surface.
std::unique_ptr<Surface> polished(std::unique_ptr<Surface> surface,
                                  const std::vector<Vector3d>& points, int steps) {
    const int parameters = surface->parameters();
    const auto rows = static_cast<Eigen::Index>(points.size());
    if (rows <= parameters) {
        return surface;
    }
    constexpr double h = 1e-6;
    Eigen::VectorXd residual(rows);
    Eigen::MatrixXd jacobian(rows, parameters);
    const auto residuals = [&](const Surface& s, Eigen::VectorXd& out) {
        for (Eigen::Index i = 0; i < rows; ++i) {
            out(i) = s.distance(points[static_cast<std::size_t>(i)]);
        }
    };
    for (int step = 0; step < steps; ++step) {
        residuals(*surface, residual);
        for (int p = 0; p < parameters; ++p) {
            const std::unique_ptr<Surface> near = surface->stepped(p, h);
            Eigen::VectorXd moved(rows);
            residuals(*near, moved);
            jacobian.col(p) = (moved - residual) / h;
        }
        const Eigen::VectorXd change = jacobian.colPivHouseholderQr().solve(-residual);
        if (!change.allFinite()) {
            break;
        }
        std::unique_ptr<Surface> next = std::move(surface);
        for (int p = 0; p < parameters; ++p) {
            next = next->stepped(p, change(p));
        }
        surface = std::move(next);
        if (change.norm() < 1e-10) {
            break;
        }
    }
    return surface;
}

class Recognizer {
public:
    Recognizer(const Mesh& mesh, double tolerance) : m_mesh(mesh), m_tolerance(tolerance) {
        m_points.reserve(mesh.vertices.size());
        for (const Point& p : mesh.vertices) {
            m_points.push_back(vec(p));
        }
        Vector3d low = Vector3d::Constant(std::numeric_limits<double>::max());
        Vector3d high = Vector3d::Constant(std::numeric_limits<double>::lowest());
        for (const Vector3d& p : m_points) {
            low = low.cwiseMin(p);
            high = high.cwiseMax(p);
        }
        m_size = (high - low).norm();
    }

    MeshSurfaces run() {
        trianglesAndEdges();
        flatPatches();
        curvedSurfaces();
        return result();
    }

private:
    using Fit = std::function<std::unique_ptr<Surface>(const std::vector<Index>&)>;

    struct Found {
        std::unique_ptr<Surface> surface;
        std::vector<Index> patches;
    };

    void checkStop() const {
        if (stopRequested()) {
            throw Error("find the surfaces of a mesh", "it was stopped");
        }
    }

    void trianglesAndEdges() {
        const std::size_t count = m_mesh.triangles.size();
        m_normals.resize(count);
        m_areas.resize(count);
        for (std::size_t t = 0; t < count; ++t) {
            const auto& c = m_mesh.triangles[t];
            const Vector3d cross =
                (m_points[c[1]] - m_points[c[0]]).cross(m_points[c[2]] - m_points[c[0]]);
            const double length = cross.norm();
            m_areas[t] = length / 2.0;
            m_normals[t] = length > 0.0 ? Vector3d(cross / length) : Vector3d::Zero();
        }
        struct Side {
            Index low, high, triangle, opposite;
        };
        std::vector<Side> sides;
        sides.reserve(count * 3);
        for (std::size_t t = 0; t < count; ++t) {
            const auto& c = m_mesh.triangles[t];
            for (std::size_t k = 0; k < 3; ++k) {
                const Index a = c[k];
                const Index b = c[(k + 1) % 3];
                sides.push_back({std::min(a, b), std::max(a, b), static_cast<Index>(t), c[(k + 2) % 3]});
            }
        }
        std::sort(sides.begin(), sides.end(), [](const Side& a, const Side& b) {
            return std::tie(a.low, a.high) < std::tie(b.low, b.high);
        });
        for (std::size_t i = 0; i < sides.size();) {
            std::size_t j = i + 1;
            while (j < sides.size() && sides[j].low == sides[i].low && sides[j].high == sides[i].high) {
                ++j;
            }
            // Where three or more triangles share an edge, the surface goes no further.
            if (j - i == 2) {
                m_edges.push_back({sides[i].triangle, sides[i + 1].triangle, sides[i].opposite,
                                   sides[i + 1].opposite});
            }
            i = j;
        }
    }

    void flatPatches() {
        const auto count = static_cast<Index>(m_mesh.triangles.size());
        // The sides of each triangle lead to the triangles beside it.
        std::vector<Index> offsets(static_cast<std::size_t>(count) + 1, 0);
        for (const Edge& e : m_edges) {
            ++offsets[e.a + 1];
            ++offsets[e.b + 1];
        }
        std::partial_sum(offsets.begin(), offsets.end(), offsets.begin());
        std::vector<Index> beside(offsets.back());
        std::vector<Index> filled(offsets.begin(), offsets.end() - 1);
        for (const Edge& e : m_edges) {
            beside[filled[e.a]++] = e.b;
            beside[filled[e.b]++] = e.a;
        }

        // Each patch keeps to the plane of the largest triangle in it. Letting each triangle
        // answer only to its neighbour would carry a finely cut cylinder all the way round, every
        // step being small.
        std::vector<Index> order(count);
        std::iota(order.begin(), order.end(), Index{0});
        std::sort(order.begin(), order.end(),
                  [&](Index a, Index b) { return m_areas[a] > m_areas[b]; });
        m_patchOf.assign(count, none);
        std::vector<Index> stack;
        const double sameFacing = std::cos(flatAngle);
        for (const Index seed : order) {
            if (m_patchOf[seed] != none) {
                continue;
            }
            const auto number = static_cast<Index>(m_patches.size());
            m_patches.emplace_back();
            Patch& patch = m_patches.back();
            const Vector3d normal = m_normals[seed];
            const Vector3d on = m_points[m_mesh.triangles[seed][0]];
            m_patchOf[seed] = number;
            stack.push_back(seed);
            while (!stack.empty()) {
                const Index t = stack.back();
                stack.pop_back();
                patch.triangles.push_back(t);
                patch.area += m_areas[t];
                patch.normal += m_normals[t] * m_areas[t];
                const auto& c = m_mesh.triangles[t];
                patch.middle +=
                    (m_points[c[0]] + m_points[c[1]] + m_points[c[2]]) / 3.0 * m_areas[t];
                for (Index i = offsets[t]; i < offsets[t + 1]; ++i) {
                    const Index other = beside[i];
                    if (m_patchOf[other] != none) {
                        continue;
                    }
                    const auto& o = m_mesh.triangles[other];
                    const bool inPlane = std::all_of(o.begin(), o.end(), [&](Index corner) {
                        return std::abs((m_points[corner] - on).dot(normal)) < m_tolerance;
                    });
                    // The normal of a sliver is mostly rounding, but where its corners are is not.
                    if (inPlane && (isSliver(other) || m_normals[other].dot(normal) > sameFacing)) {
                        m_patchOf[other] = number;
                        stack.push_back(other);
                    }
                }
            }
        }
        for (Patch& patch : m_patches) {
            if (patch.area > 0.0) {
                patch.middle /= patch.area;
            }
            if (patch.normal.norm() > 0.0) {
                patch.normal.normalize();
            }
            for (const Index t : patch.triangles) {
                const auto& c = m_mesh.triangles[t];
                patch.corners.insert(patch.corners.end(), c.begin(), c.end());
            }
            std::sort(patch.corners.begin(), patch.corners.end());
            patch.corners.erase(std::unique(patch.corners.begin(), patch.corners.end()),
                                patch.corners.end());
        }
        for (const Edge& e : m_edges) {
            const Index a = m_patchOf[e.a];
            const Index b = m_patchOf[e.b];
            if (a == b) {
                continue;
            }
            const double angle =
                std::acos(std::clamp(m_normals[e.a].dot(m_normals[e.b]), -1.0, 1.0));
            const auto add = [&](Index from, Index to) {
                for (Patch::Neighbour& n : m_patches[from].neighbours) {
                    if (n.patch == to) {
                        n.angle = std::min(n.angle, angle);
                        return;
                    }
                }
                m_patches[from].neighbours.push_back({to, angle});
            };
            add(a, b);
            add(b, a);
        }
    }

    // Whether a triangle is so thin that which way it faces cannot be told.
    bool isSliver(Index t) const {
        const auto& c = m_mesh.triangles[t];
        const double longest = std::max({(m_points[c[1]] - m_points[c[0]]).norm(),
                                         (m_points[c[2]] - m_points[c[1]]).norm(),
                                         (m_points[c[0]] - m_points[c[2]]).norm()});
        return m_areas[t] * 2.0 < longest * m_tolerance * 4.0;
    }

    std::vector<Vector3d> cornersOf(const std::vector<Index>& patches) const {
        std::vector<Index> corners;
        for (const Index p : patches) {
            corners.insert(corners.end(), m_patches[p].corners.begin(), m_patches[p].corners.end());
        }
        std::sort(corners.begin(), corners.end());
        corners.erase(std::unique(corners.begin(), corners.end()), corners.end());
        std::vector<Vector3d> points;
        points.reserve(corners.size());
        for (const Index c : corners) {
            points.push_back(m_points[c]);
        }
        return points;
    }

    bool fits(const Surface& surface, const std::vector<Vector3d>& points) const {
        const double limit = m_tolerance * growSlack;
        return std::all_of(points.begin(), points.end(), [&](const Vector3d& p) {
            return std::abs(surface.distance(p)) < limit;
        });
    }

    std::unique_ptr<Surface> fitCylinder(const std::vector<Index>& patches) const {
        const std::vector<Vector3d> points = cornersOf(patches);
        if (points.size() < 6) {
            return nullptr;
        }
        Eigen::Matrix3d spread = Eigen::Matrix3d::Zero();
        for (const Index p : patches) {
            spread += m_patches[p].area * m_patches[p].normal * m_patches[p].normal.transpose();
        }
        const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(spread);
        const Vector3d axis = solver.eigenvectors().col(0);
        const auto [u, v] = basisAround(axis);
        Eigen::MatrixX2d flat(static_cast<Eigen::Index>(points.size()), 2);
        for (std::size_t i = 0; i < points.size(); ++i) {
            flat.row(static_cast<Eigen::Index>(i)) << points[i].dot(u), points[i].dot(v);
        }
        const auto circle = circleThrough(flat);
        if (!circle || circle->second > m_size * largestRadius) {
            return nullptr;
        }
        return std::make_unique<Cylinder>(axis, u * circle->first.x() + v * circle->first.y(),
                                          circle->second);
    }

    std::unique_ptr<Surface> fitCone(const std::vector<Index>& patches) const {
        if (patches.size() < 3) {
            return nullptr;
        }
        double total = 0.0;
        Vector3d mean = Vector3d::Zero();
        for (const Index p : patches) {
            total += m_patches[p].area;
            mean += m_patches[p].normal * m_patches[p].area;
        }
        if (total <= 0.0) {
            return nullptr;
        }
        mean /= total;
        Eigen::Matrix3d spread = Eigen::Matrix3d::Zero();
        for (const Index p : patches) {
            const Vector3d d = m_patches[p].normal - mean;
            spread += m_patches[p].area * d * d.transpose();
        }
        const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(spread);
        Vector3d axis = solver.eigenvectors().col(0);
        const auto rows = static_cast<Eigen::Index>(patches.size());
        // The planes of a cone's facets all pass through its apex.
        Eigen::MatrixX3d planes(rows, 3);
        Eigen::VectorXd offsets(rows);
        for (Eigen::Index i = 0; i < rows; ++i) {
            const Patch& patch = m_patches[patches[static_cast<std::size_t>(i)]];
            planes.row(i) = patch.normal.transpose() * std::sqrt(patch.area);
            offsets(i) = patch.normal.dot(patch.middle) * std::sqrt(patch.area);
        }
        const Vector3d apex = planes.colPivHouseholderQr().solve(offsets);
        if (!apex.allFinite() || (apex - m_points[m_patches[patches[0]].corners[0]]).norm() >
                                     m_size * largestRadius) {
            return nullptr;
        }
        // Along the axis from the apex to the surface, whichever way the mesh faces.
        const std::vector<Vector3d> points = cornersOf(patches);
        Vector3d middle = Vector3d::Zero();
        for (const Vector3d& p : points) {
            middle += p;
        }
        middle /= static_cast<double>(points.size());
        if ((middle - apex).dot(axis) < 0.0) {
            axis = -axis;
        }
        const double sine = std::abs(mean.dot(axis));
        if (sine < 2e-3 || sine > 0.985) {
            return nullptr;
        }
        return std::make_unique<Cone>(axis, apex, std::asin(sine));
    }

    std::unique_ptr<Surface> fitSphere(const std::vector<Index>& patches) const {
        const std::vector<Vector3d> points = cornersOf(patches);
        if (points.size() < 5) {
            return nullptr;
        }
        const auto rows = static_cast<Eigen::Index>(points.size());
        Eigen::MatrixX4d a(rows, 4);
        Eigen::VectorXd b(rows);
        for (Eigen::Index i = 0; i < rows; ++i) {
            const Vector3d& p = points[static_cast<std::size_t>(i)];
            a.row(i) << 2.0 * p.x(), 2.0 * p.y(), 2.0 * p.z(), 1.0;
            b(i) = p.squaredNorm();
        }
        const Eigen::Vector4d s = a.colPivHouseholderQr().solve(b);
        const Vector3d center = s.head<3>();
        const double squared = s(3) + center.squaredNorm();
        if (!center.allFinite() || !std::isfinite(squared) || squared <= 0.0) {
            return nullptr;
        }
        const double radius = std::sqrt(squared);
        return radius > m_size * largestRadius ? nullptr
                                               : std::make_unique<Sphere>(center, radius);
    }

    // The axis of a surface of revolution is the line that all its normals meet (Pottmann and
    // Randrup). Written as a linear complex that is a small eigenvalue problem.
    std::unique_ptr<Surface> fitTorus(const std::vector<Index>& patches) const {
        const std::vector<Vector3d> points = cornersOf(patches);
        if (points.size() < 9) {
            return nullptr;
        }
        Eigen::Matrix<double, 6, 6> s = Eigen::Matrix<double, 6, 6>::Zero();
        for (const Index p : patches) {
            Eigen::Matrix<double, 6, 1> line;
            line.head<3>() = m_patches[p].normal;
            line.tail<3>() = m_patches[p].middle.cross(m_patches[p].normal);
            s += m_patches[p].area * line * line.transpose();
        }
        const Eigen::Matrix3d a = s.topLeftCorner<3, 3>();
        const Eigen::Matrix3d b = s.topRightCorner<3, 3>();
        const Eigen::Matrix3d c = s.bottomRightCorner<3, 3>();
        const Eigen::Matrix3d inverse =
            a.completeOrthogonalDecomposition().pseudoInverse();
        const Eigen::Matrix3d schur = c - b.transpose() * inverse * b;
        const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(schur);
        Vector3d axis = solver.eigenvectors().col(0);
        Vector3d through = axis.cross(Vector3d(-inverse * b * axis));
        if (!axis.allFinite() || !through.allFinite()) {
            return nullptr;
        }
        Eigen::MatrixX2d flat(static_cast<Eigen::Index>(points.size()), 2);
        for (std::size_t i = 0; i < points.size(); ++i) {
            const Vector3d d = points[i] - through;
            const double along = d.dot(axis);
            flat.row(static_cast<Eigen::Index>(i)) << radial(d, axis).norm(), along;
        }
        const auto circle = circleThrough(flat);
        if (!circle) {
            return nullptr;
        }
        std::unique_ptr<Surface> torus = std::make_unique<Torus>(
            axis, through + axis * circle->first.y(), circle->first.x(), circle->second);
        torus = polished(std::move(torus), points, 4);
        MeshSurface shape;
        torus->describe(shape);
        // Past these it has stopped being a torus: a flat or a nearly straight piece, or a shape
        // that goes through itself.
        if (!std::isfinite(shape.radius) || !std::isfinite(shape.tubeRadius) ||
            shape.tubeRadius <= 0.0 || shape.radius <= shape.tubeRadius ||
            shape.radius > m_size * largestRadius) {
            return nullptr;
        }
        return torus;
    }

    // Takes in neighbouring flat pieces for as long as all their corners stay on the surface
    // and they face the way it does there.
    Found grown(std::vector<Index> seed, const Fit& fit, const std::vector<char>& taken) {
        Found found;
        found.surface = fit(seed);
        if (found.surface == nullptr || !fits(*found.surface, cornersOf(seed))) {
            found.surface = nullptr;
            return found;
        }
        ++m_stamp;
        std::vector<Index> frontier;
        const auto look = [&](Index patch) {
            for (const Patch::Neighbour& n : m_patches[patch].neighbours) {
                if (m_seen[n.patch] != m_stamp && taken[n.patch] == 0) {
                    m_seen[n.patch] = m_stamp;
                    frontier.push_back(n.patch);
                }
            }
        };
        for (const Index p : seed) {
            m_seen[p] = m_stamp;
        }
        found.patches = seed;
        for (const Index p : seed) {
            look(p);
        }
        std::size_t sinceFit = 0;
        const double limit = m_tolerance * growSlack;
        const double lean = std::cos(leanAngle);
        while (!frontier.empty()) {
            const Index candidate = frontier.back();
            frontier.pop_back();
            const Patch& patch = m_patches[candidate];
            const bool onIt = std::all_of(
                patch.corners.begin(), patch.corners.end(), [&](Index c) {
                    return std::abs(found.surface->distance(m_points[c])) < limit;
                });
            if (!onIt) {
                continue;
            }
            const bool facesIt = std::all_of(
                patch.triangles.begin(), patch.triangles.end(), [&](Index t) {
                    const auto& c = m_mesh.triangles[t];
                    const Vector3d middle = (m_points[c[0]] + m_points[c[1]] + m_points[c[2]]) / 3.0;
                    return m_areas[t] == 0.0 ||
                           std::abs(found.surface->normal(middle).dot(m_normals[t])) > lean;
                });
            if (!facesIt) {
                continue;
            }
            found.patches.push_back(candidate);
            look(candidate);
            if (++sinceFit >= std::max<std::size_t>(4, found.patches.size() / 2)) {
                sinceFit = 0;
                std::unique_ptr<Surface> better = fit(found.patches);
                if (better != nullptr && fits(*better, cornersOf(found.patches))) {
                    found.surface = std::move(better);
                    // A better fit may take in what a poorer one turned away.
                    ++m_stamp;
                    for (const Index p : found.patches) {
                        m_seen[p] = m_stamp;
                    }
                    frontier.clear();
                    for (const Index p : found.patches) {
                        look(p);
                    }
                }
            }
        }
        return found;
    }

    double areaOf(const std::vector<Index>& patches) const {
        double area = 0.0;
        for (const Index p : patches) {
            area += m_patches[p].area;
        }
        return area;
    }

    void curvedSurfaces() {
        const auto count = static_cast<Index>(m_patches.size());
        m_seen.assign(count, 0);
        std::vector<char> taken(count, 0);
        m_surfaceOfPatch.assign(count, none);
        std::vector<Index> order(count);
        std::iota(order.begin(), order.end(), Index{0});
        // From the smallest up: the seeds that lie inside a curve come first, and a large flat
        // face is the last thing that could be mistaken for one.
        std::sort(order.begin(), order.end(),
                  [&](Index a, Index b) { return m_patches[a].area < m_patches[b].area; });

        const Fit cylinder = [this](const std::vector<Index>& p) { return fitCylinder(p); };
        const Fit sphere = [this](const std::vector<Index>& p) { return fitSphere(p); };
        const Fit cone = [this](const std::vector<Index>& p) { return fitCone(p); };
        const Fit torus = [this](const std::vector<Index>& p) { return fitTorus(p); };
        struct Candidate {
            const Fit* fit;
            std::size_t fewest;
            bool wide;
        };
        const std::array<Candidate, 4> candidates{{{&cylinder, 2, false},
                                                   {&sphere, 4, true},
                                                   {&cone, 3, false},
                                                   {&torus, 6, true}}};

        std::size_t done = 0;
        for (const Index p : order) {
            if ((++done & 0xFF) == 0) {
                checkStop();
            }
            if (taken[p] != 0) {
                continue;
            }
            const double sizeLimit = m_patches[p].area * seedSizeRatio + 1e-12;
            std::vector<Index> near{p};
            for (const Patch::Neighbour& n : m_patches[p].neighbours) {
                if (n.angle < smoothAngle && taken[n.patch] == 0 &&
                    m_patches[n.patch].area < sizeLimit) {
                    near.push_back(n.patch);
                }
            }
            if (near.size() < 2) {
                continue;
            }
            // A second ring, for the surfaces that curve both ways and need more to go on.
            std::vector<Index> wide = near;
            for (std::size_t i = 1; i < near.size(); ++i) {
                for (const Patch::Neighbour& n : m_patches[near[i]].neighbours) {
                    if (n.angle < smoothAngle && taken[n.patch] == 0 &&
                        m_patches[n.patch].area < sizeLimit &&
                        std::find(wide.begin(), wide.end(), n.patch) == wide.end()) {
                        wide.push_back(n.patch);
                    }
                }
            }
            Found best;
            double bestArea = 0.0;
            for (const Candidate& candidate : candidates) {
                const std::vector<Index>& seed = candidate.wide ? wide : near;
                if (seed.size() < candidate.fewest) {
                    continue;
                }
                Found found = grown(seed, *candidate.fit, taken);
                if (found.surface == nullptr || found.patches.size() < std::max<std::size_t>(candidate.fewest, 3)) {
                    continue;
                }
                const double area = areaOf(found.patches);
                // The simpler surface wins a tie: these are tried simplest first.
                if (area > bestArea * 1.02) {
                    bestArea = area;
                    best = std::move(found);
                }
            }
            if (best.surface != nullptr) {
                best.surface = polished(std::move(best.surface), sampleOf(best.patches), 6);
                if (!sensible(*best.surface) || !fits(*best.surface, cornersOf(best.patches))) {
                    continue;
                }
                const auto number = static_cast<Index>(m_found.size());
                for (const Index q : best.patches) {
                    taken[q] = 1;
                    m_surfaceOfPatch[q] = number;
                }
                m_found.push_back(std::move(best));
            }
        }
    }

    // A sample of the corners is all a fit to a large surface needs.
    std::vector<Vector3d> sampleOf(const std::vector<Index>& patches) const {
        const std::vector<Vector3d> points = cornersOf(patches);
        std::vector<Vector3d> sample;
        const std::size_t step = std::max<std::size_t>(1, points.size() / 4000);
        for (std::size_t i = 0; i < points.size(); i += step) {
            sample.push_back(points[i]);
        }
        return sample;
    }

    // A surface that has grown far larger than the mesh is a flat piece, or nearly so.
    bool sensible(const Surface& surface) const {
        MeshSurface shape;
        surface.describe(shape);
        const double limit = m_size * largestRadius;
        switch (shape.kind) {
        case SurfaceKind::Cylinder:
        case SurfaceKind::Sphere:
            return shape.radius > 0.0 && shape.radius < limit;
        case SurfaceKind::Torus:
            return shape.tubeRadius > 0.0 && shape.radius > shape.tubeRadius &&
                   shape.radius < limit;
        case SurfaceKind::Cone:
            return shape.halfAngle > 1e-3 && shape.halfAngle < pi / 2.0 - 1e-3;
        case SurfaceKind::Plane:
            return true;
        }
        return false;
    }

    MeshSurface finished(const Found& found) const {
        MeshSurface out;
        const Surface* const surface = found.surface.get();
        out.kind = surface->kind();
        surface->describe(out);
        double facing = 0.0;
        for (const Index p : found.patches) {
            for (const Index t : m_patches[p].triangles) {
                const auto& c = m_mesh.triangles[t];
                const Vector3d middle = (m_points[c[0]] + m_points[c[1]] + m_points[c[2]]) / 3.0;
                facing += surface->normal(middle).dot(m_normals[t]) * m_areas[t];
                out.triangles.push_back(t);
            }
            out.area += m_patches[p].area;
        }
        out.outward = facing >= 0.0;
        return out;
    }

    MeshSurface flat(Index p) const {
        const Patch& patch = m_patches[p];
        MeshSurface out;
        out.kind = SurfaceKind::Plane;
        Vector3d middle = Vector3d::Zero();
        Eigen::Matrix3d spread = Eigen::Matrix3d::Zero();
        for (const Index c : patch.corners) {
            middle += m_points[c];
        }
        middle /= static_cast<double>(patch.corners.size());
        for (const Index c : patch.corners) {
            const Vector3d d = m_points[c] - middle;
            spread += d * d.transpose();
        }
        const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(spread);
        Vector3d normal = solver.eigenvectors().col(0);
        if (normal.dot(patch.normal) < 0.0) {
            normal = -normal;
        }
        // Three corners say nothing a triangle's own normal does not, and it is less affected by
        // slivers.
        if (patch.corners.size() <= 3 && patch.normal.norm() > 0.0) {
            normal = patch.normal;
        }
        out.origin = point(middle);
        out.axis = direction(normal);
        out.triangles = patch.triangles;
        out.area = patch.area;
        int smooth = 0;
        for (const Patch::Neighbour& n : patch.neighbours) {
            smooth += n.angle < smoothAngle ? 1 : 0;
        }
        out.facet = smooth >= 2 && patch.area < m_totalArea * 0.02;
        return out;
    }

    MeshSurfaces result() {
        MeshSurfaces out;
        out.tolerance = m_tolerance;
        out.surfaceOf.assign(m_mesh.triangles.size(), none);
        m_totalArea = std::accumulate(m_areas.begin(), m_areas.end(), 0.0);
        for (const Found& found : m_found) {
            out.surfaces.push_back(finished(found));
        }
        for (Index p = 0; p < m_patches.size(); ++p) {
            if (m_surfaceOfPatch[p] == none) {
                out.surfaces.push_back(flat(p));
            }
        }
        double real = 0.0;
        for (std::size_t s = 0; s < out.surfaces.size(); ++s) {
            for (const Index t : out.surfaces[s].triangles) {
                out.surfaceOf[t] = static_cast<Index>(s);
            }
            real += out.surfaces[s].facet ? 0.0 : out.surfaces[s].area;
        }
        out.share = m_totalArea > 0.0 ? real / m_totalArea : 0.0;
        return out;
    }

    struct Edge {
        Index a, b, tipA, tipB;
    };

    const Mesh& m_mesh;
    double m_tolerance;
    double m_size = 0.0;
    double m_totalArea = 0.0;
    std::vector<Vector3d> m_points;
    std::vector<Vector3d> m_normals;
    std::vector<double> m_areas;
    std::vector<Edge> m_edges;
    std::vector<Patch> m_patches;
    std::vector<Index> m_patchOf;
    std::vector<Index> m_surfaceOfPatch;
    std::vector<Found> m_found;
    std::vector<Index> m_seen;
    Index m_stamp = 0;
};

} // namespace

double defaultMeshTolerance(const Mesh& mesh) {
    Point low{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
              std::numeric_limits<double>::max()};
    Point high{std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(),
               std::numeric_limits<double>::lowest()};
    double largest = 0.0;
    for (const Point& p : mesh.vertices) {
        low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
        largest = std::max({largest, std::abs(p.x), std::abs(p.y), std::abs(p.z)});
    }
    // The first term is the rounding of single precision numbers, the second what the corners of
    // a mesh that has been through another program or two are off by.
    return std::max(largest * 4e-7, std::hypot(high.x - low.x, high.y - low.y, high.z - low.z) * 3e-5);
}

MeshSurfaces findSurfaces(const Mesh& mesh, double tolerance) {
    if (mesh.triangles.empty()) {
        throw Error("find the surfaces of a mesh", "it has no triangles");
    }
    return Recognizer(mesh, tolerance > 0.0 ? tolerance : defaultMeshTolerance(mesh)).run();
}

} // namespace lcad::kernel
