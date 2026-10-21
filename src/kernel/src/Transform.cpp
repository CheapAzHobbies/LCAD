// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Transform.h>

#include <lcad/kernel/occt/Guard.h>
#include <lcad/kernel/occt/PlacementAccess.h>
#include <lcad/kernel/occt/ShapeAccess.h>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <stdexcept>

namespace lcad::kernel {

namespace {

Vector plus(const Vector& a, const Vector& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vector scaled(const Vector& v, double by) {
    return {v.x * by, v.y * by, v.z * by};
}

double dot(const Vector& a, const Vector& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vector cross(const Vector& a, const Vector& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

double norm(const Vector& v) {
    return std::sqrt(dot(v, v));
}

Vector unit(const Vector& v) {
    const double length = norm(v);
    return {v.x / length, v.y / length, v.z / length};
}

Vector toVector(const Point& p) {
    return {p.x, p.y, p.z};
}

Point toPoint(const Vector& v) {
    return {v.x, v.y, v.z};
}

bool finite(const Vector& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

Vector fromOcct(const gp_Dir& direction) {
    return {direction.X(), direction.Y(), direction.Z()};
}

Point fromOcct(const gp_Pnt& point) {
    return {point.X(), point.Y(), point.Z()};
}

} // namespace

Shape translated(const Shape& shape, double dx, double dy, double dz) {
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz)) {
        throw std::invalid_argument("translated: the distances must be finite numbers");
    }
    if (shape.isNull()) {
        return shape;
    }
    return occt::guarded("move shape", [&] {
        gp_Trsf transform;
        transform.SetTranslation(gp_Vec(dx, dy, dz));
        return occt::wrap(occt::unwrap(shape).Moved(TopLoc_Location(transform)));
    });
}

Placement operator*(const Placement& outer, const Placement& inner) {
    return {apply(outer, inner.origin), apply(outer, inner.xAxis), apply(outer, inner.yAxis),
            apply(outer, inner.zAxis)};
}

Placement inverse(const Placement& placement) {
    // The directions are square and of unit length, so turning back is turning by the
    // transpose.
    const Vector& x = placement.xAxis;
    const Vector& y = placement.yAxis;
    const Vector& z = placement.zAxis;
    Placement back;
    back.xAxis = {x.x, y.x, z.x};
    back.yAxis = {x.y, y.y, z.y};
    back.zAxis = {x.z, y.z, z.z};
    const Vector turned = apply(back, toVector(placement.origin));
    back.origin = {-turned.x, -turned.y, -turned.z};
    return back;
}

Point apply(const Placement& placement, const Point& point) {
    return toPoint(plus(toVector(placement.origin), apply(placement, toVector(point))));
}

Vector apply(const Placement& placement, const Vector& direction) {
    return plus(plus(scaled(placement.xAxis, direction.x), scaled(placement.yAxis, direction.y)),
                scaled(placement.zAxis, direction.z));
}

Plane apply(const Placement& placement, const Plane& plane) {
    return {apply(placement, plane.origin), apply(placement, plane.normal),
            apply(placement, plane.xDirection)};
}

Axis apply(const Placement& placement, const Axis& axis) {
    return {apply(placement, axis.origin), apply(placement, axis.direction)};
}

Placement translation(const Vector& by) {
    Placement moved;
    moved.origin = toPoint(by);
    return moved;
}

Placement rotation(const Axis& about, double angle) {
    if (norm(about.direction) == 0.0 || !finite(about.direction) || !std::isfinite(angle)) {
        throw std::invalid_argument("rotation: the axis needs a direction, and the angle a value");
    }
    const Vector d = unit(about.direction);
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    // Rodrigues: v turned about d is v cos + (d x v) sin + d (d . v)(1 - cos).
    const auto turn = [&](const Vector& v) {
        return plus(plus(scaled(v, c), scaled(cross(d, v), s)), scaled(d, dot(d, v) * (1.0 - c)));
    };
    Placement turned;
    turned.xAxis = turn({1.0, 0.0, 0.0});
    turned.yAxis = turn({0.0, 1.0, 0.0});
    turned.zAxis = turn({0.0, 0.0, 1.0});
    // The line stays where it is: its point goes to itself.
    const Vector a = toVector(about.origin);
    turned.origin = toPoint(plus(a, scaled(apply(turned, a), -1.0)));
    return turned;
}

Placement frameOf(const Plane& plane) {
    const Vector z = unit(plane.normal);
    const Vector x = unit(plane.xDirection);
    return {plane.origin, x, cross(z, x), z};
}

Placement frameAt(const Point& origin, const Vector& z) {
    if (norm(z) == 0.0 || !finite(z)) {
        throw std::invalid_argument("frameAt: Z needs a direction");
    }
    const Vector up = unit(z);
    Vector x = plus(Vector{1.0, 0.0, 0.0}, scaled(up, -up.x));
    if (norm(x) < 1e-6) {
        x = plus(Vector{0.0, 1.0, 0.0}, scaled(up, -up.y));
    }
    x = unit(x);
    return {origin, x, cross(up, x), up};
}

bool samePlacement(const Placement& a, const Placement& b, double tolerance) {
    const auto close = [tolerance](const Vector& u, const Vector& v) {
        return norm(plus(u, scaled(v, -1.0))) <= tolerance;
    };
    return close(toVector(a.origin), toVector(b.origin)) && close(a.xAxis, b.xAxis) &&
           close(a.yAxis, b.yAxis) && close(a.zAxis, b.zAxis);
}

bool isRigid(const Placement& placement) {
    const Vector& x = placement.xAxis;
    const Vector& y = placement.yAxis;
    const Vector& z = placement.zAxis;
    if (!finite(toVector(placement.origin)) || !finite(x) || !finite(y) || !finite(z)) {
        return false;
    }
    constexpr double tolerance = 1e-6;
    const auto nearly = [](double value, double expected) {
        return std::abs(value - expected) <= tolerance;
    };
    return nearly(norm(x), 1.0) && nearly(norm(y), 1.0) && nearly(norm(z), 1.0) &&
           nearly(dot(x, y), 0.0) && nearly(dot(y, z), 0.0) && nearly(dot(z, x), 0.0) &&
           nearly(dot(cross(x, y), z), 1.0);
}

Placement normalized(const Placement& placement) {
    if (norm(placement.zAxis) == 0.0 || !finite(placement.zAxis)) {
        throw std::invalid_argument("normalized: Z needs a direction");
    }
    const Vector z = unit(placement.zAxis);
    const Vector x = plus(placement.xAxis, scaled(z, -dot(z, placement.xAxis)));
    if (norm(x) < 1e-12) {
        throw std::invalid_argument("normalized: X lies along Z");
    }
    const Vector xUnit = unit(x);
    return {placement.origin, xUnit, cross(z, xUnit), z};
}

Shape placed(const Shape& shape, const Placement& placement) {
    if (shape.isNull()) {
        return shape;
    }
    if (!isRigid(placement)) {
        throw std::invalid_argument("placed: the placement turns and moves, and does no more");
    }
    return occt::guarded("place shape", [&] {
        return occt::wrap(occt::unwrap(shape).Moved(TopLoc_Location(occt::toTrsf(placement))));
    });
}

std::optional<Placement> frameOn(const Shape& part, FrameSnap snap) {
    if (part.isNull()) {
        return std::nullopt;
    }
    return occt::guarded("find a frame on a shape", [&]() -> std::optional<Placement> {
        const TopoDS_Shape& shape = occt::unwrap(part);
        // Of a stretch of line from `first` to `last` along `direction` from `base`: the point
        // the snap picks.
        const auto along = [snap](const gp_Pnt& base, const gp_Dir& direction, double first,
                                  double last) {
            const double t = snap == FrameSnap::Start ? first
                             : snap == FrameSnap::End ? last
                                                      : 0.5 * (first + last);
            return fromOcct(base.Translated(gp_Vec(direction) * t));
        };
        switch (shape.ShapeType()) {
        case TopAbs_VERTEX:
            return frameAt(fromOcct(BRep_Tool::Pnt(TopoDS::Vertex(shape))), {0.0, 0.0, 1.0});
        case TopAbs_EDGE: {
            const BRepAdaptor_Curve curve(TopoDS::Edge(shape));
            const double first = curve.FirstParameter();
            const double last = curve.LastParameter();
            if (curve.GetType() == GeomAbs_Circle) {
                const gp_Circ circle = curve.Circle();
                return frameAt(fromOcct(circle.Location()), fromOcct(circle.Axis().Direction()));
            }
            if (curve.GetType() == GeomAbs_Line) {
                const gp_Lin line = curve.Line();
                const gp_Pnt start = curve.Value(first);
                return frameAt(along(start, line.Direction(), 0.0, last - first),
                               fromOcct(line.Direction()));
            }
            const double t = snap == FrameSnap::Start ? first
                             : snap == FrameSnap::End ? last
                                                      : 0.5 * (first + last);
            gp_Pnt at;
            gp_Vec tangent;
            curve.D1(t, at, tangent);
            if (tangent.Magnitude() < 1e-12) {
                return std::nullopt;
            }
            return frameAt(fromOcct(at), fromOcct(gp_Dir(tangent)));
        }
        case TopAbs_FACE: {
            const TopoDS_Face& face = TopoDS::Face(shape);
            const BRepAdaptor_Surface surface(face);
            double uMin = 0.0;
            double uMax = 0.0;
            double vMin = 0.0;
            double vMax = 0.0;
            BRepTools::UVBounds(face, uMin, uMax, vMin, vMax);
            switch (surface.GetType()) {
            case GeomAbs_Plane: {
                const std::optional<Plane> plane = planeOfFace(part);
                if (!plane.has_value()) {
                    return std::nullopt;
                }
                GProp_GProps properties;
                BRepGProp::SurfaceProperties(shape, properties);
                return frameAt(fromOcct(properties.CentreOfMass()), plane->normal);
            }
            case GeomAbs_Cylinder: {
                // V runs along the axis from the cylinder's own origin.
                const gp_Ax1 axis = surface.Cylinder().Axis();
                return frameAt(along(axis.Location(), axis.Direction(), vMin, vMax),
                               fromOcct(axis.Direction()));
            }
            case GeomAbs_Cone: {
                // V runs along the cone's side, which leans from the axis by its half angle.
                const gp_Cone cone = surface.Cone();
                const double c = std::cos(cone.SemiAngle());
                const gp_Ax1 axis = cone.Axis();
                return frameAt(along(axis.Location(), axis.Direction(), vMin * c, vMax * c),
                               fromOcct(axis.Direction()));
            }
            case GeomAbs_Sphere:
                return frameAt(fromOcct(surface.Sphere().Location()),
                               fromOcct(surface.Sphere().Position().Direction()));
            case GeomAbs_Torus:
                return frameAt(fromOcct(surface.Torus().Location()),
                               fromOcct(surface.Torus().Axis().Direction()));
            default: {
                gp_Pnt at;
                gp_Vec du;
                gp_Vec dv;
                surface.D1(0.5 * (uMin + uMax), 0.5 * (vMin + vMax), at, du, dv);
                gp_Vec normal = du.Crossed(dv);
                if (normal.Magnitude() < 1e-12) {
                    return std::nullopt;
                }
                if (face.Orientation() == TopAbs_REVERSED) {
                    normal.Reverse();
                }
                return frameAt(fromOcct(at), fromOcct(gp_Dir(normal)));
            }
            }
        }
        default:
            return std::nullopt;
        }
    });
}

} // namespace lcad::kernel
