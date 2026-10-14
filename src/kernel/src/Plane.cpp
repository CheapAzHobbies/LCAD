// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Plane.h>

#include <lcad/kernel/occt/Guard.h>
#include <lcad/kernel/occt/ShapeAccess.h>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Geom_BSplineCurve.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Circ.hxx>
#include <gp_Elips.hxx>
#include <gp_Pln.hxx>

#include <cmath>
#include <initializer_list>
#include <stdexcept>
#include <string>

namespace lcad::kernel {

namespace {

double dot(const Vector& a, const Vector& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vector cross(const Vector& a, const Vector& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vector normalized(const Vector& v, const char* what) {
    const double length = std::sqrt(dot(v, v));
    if (!std::isfinite(length) || length < 1e-12) {
        throw std::invalid_argument(std::string("makePlane: the ") + what +
                                    " is not a usable direction");
    }
    return {v.x / length, v.y / length, v.z / length};
}

gp_Pnt toOcct(const Point& p) {
    return gp_Pnt(p.x, p.y, p.z);
}

gp_Ax2 axesOf(const Plane& plane, double centerU, double centerV) {
    const Point center = toWorld(plane, centerU, centerV);
    return gp_Ax2(toOcct(center), gp_Dir(plane.normal.x, plane.normal.y, plane.normal.z),
                  gp_Dir(plane.xDirection.x, plane.xDirection.y, plane.xDirection.z));
}

void requireFinite(std::initializer_list<double> values, const char* function) {
    for (const double value : values) {
        if (!std::isfinite(value)) {
            throw std::invalid_argument(std::string(function) + ": a value is not finite");
        }
    }
}

} // namespace

Plane makePlane(const Point& origin, const Vector& normal, const Vector& xDirection) {
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z)) {
        throw std::invalid_argument("makePlane: the origin is not finite");
    }
    Plane plane;
    plane.origin = origin;
    plane.normal = normalized(normal, "normal");
    plane.xDirection = normalized(xDirection, "x direction");
    if (std::abs(dot(plane.normal, plane.xDirection)) > 1e-6) {
        throw std::invalid_argument("makePlane: the x direction is not square to the normal");
    }
    return plane;
}

Plane planeXY() {
    return makePlane({0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, {1.0, 0.0, 0.0});
}

Plane planeXZ() {
    return makePlane({0.0, 0.0, 0.0}, {0.0, -1.0, 0.0}, {1.0, 0.0, 0.0});
}

Plane planeYZ() {
    return makePlane({0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0});
}

Vector yDirection(const Plane& plane) {
    return cross(plane.normal, plane.xDirection);
}

Point toWorld(const Plane& plane, double u, double v) {
    const Vector y = yDirection(plane);
    return {plane.origin.x + u * plane.xDirection.x + v * y.x,
            plane.origin.y + u * plane.xDirection.y + v * y.y,
            plane.origin.z + u * plane.xDirection.z + v * y.z};
}

PlaneCoordinates toLocal(const Plane& plane, const Point& point) {
    const Vector offset{point.x - plane.origin.x, point.y - plane.origin.y,
                        point.z - plane.origin.z};
    return {dot(offset, plane.xDirection), dot(offset, yDirection(plane)),
            dot(offset, plane.normal)};
}

std::optional<Plane> planeOfFace(const Shape& face) {
    if (face.isNull() || occt::unwrap(face).ShapeType() != TopAbs_FACE) {
        return std::nullopt;
    }
    return occt::guarded("find the plane of a face", [&]() -> std::optional<Plane> {
        const TopoDS_Face& topoFace = TopoDS::Face(occt::unwrap(face));
        const BRepAdaptor_Surface surface(topoFace);
        if (surface.GetType() != GeomAbs_Plane) {
            return std::nullopt;
        }
        const gp_Pln flat = surface.Plane();
        gp_Dir outward = flat.Axis().Direction();
        // The surface's own normal is its axis, unless its frame is left-handed, as that of a
        // mirror image is: then it is the other way.
        if (!flat.Position().Direct()) {
            outward.Reverse();
        }
        // The surface's own normal points out of the solid unless the face uses it reversed.
        if (topoFace.Orientation() == TopAbs_REVERSED) {
            outward.Reverse();
        }
        const Vector normal{outward.X(), outward.Y(), outward.Z()};
        // Where the model's origin drops onto the plane.
        const gp_Pnt on = flat.Location();
        const double height = -(on.X() * normal.x + on.Y() * normal.y + on.Z() * normal.z);
        const Point origin{-height * normal.x, -height * normal.y, -height * normal.z};
        // The model's X, flattened into the plane; or its Y, when the plane faces along X.
        Vector along{1.0, 0.0, 0.0};
        if (std::abs(normal.x) > 0.999) {
            along = {0.0, 1.0, 0.0};
        }
        const double out = dot(along, normal);
        const Vector inPlane{along.x - out * normal.x, along.y - out * normal.y,
                             along.z - out * normal.z};
        return makePlane(origin, normal, inPlane);
    });
}

Shape makeSegment(const Point& start, const Point& end) {
    requireFinite({start.x, start.y, start.z, end.x, end.y, end.z}, "makeSegment");
    if (toOcct(start).Distance(toOcct(end)) <= 1e-7) {
        throw std::invalid_argument("makeSegment: the two points coincide");
    }
    return occt::guarded("make a line", [&] {
        return occt::wrap(BRepBuilderAPI_MakeEdge(toOcct(start), toOcct(end)).Edge());
    });
}

Shape makeCircleEdge(const Plane& plane, double centerU, double centerV, double radius) {
    requireFinite({centerU, centerV, radius}, "makeCircleEdge");
    if (radius <= 1e-7) {
        throw std::invalid_argument("makeCircleEdge: the radius must be greater than zero");
    }
    return occt::guarded("make a circle", [&] {
        return occt::wrap(
            BRepBuilderAPI_MakeEdge(gp_Circ(axesOf(plane, centerU, centerV), radius)).Edge());
    });
}

Shape makeEllipseEdge(const Plane& plane, double centerU, double centerV, double majorU,
                      double majorV, double minorRadius) {
    requireFinite({centerU, centerV, majorU, majorV, minorRadius}, "makeEllipseEdge");
    const double major = std::hypot(majorU - centerU, majorV - centerV);
    if (major <= 1e-7 || minorRadius <= 1e-7) {
        throw std::invalid_argument("makeEllipseEdge: both radii must be greater than zero");
    }
    return occt::guarded("make an ellipse", [&] {
        const Point center = toWorld(plane, centerU, centerV);
        const Point end = toWorld(plane, majorU, majorV);
        const gp_Dir normal(plane.normal.x, plane.normal.y, plane.normal.z);
        gp_Dir along(end.x - center.x, end.y - center.y, end.z - center.z);
        double a = major;
        double b = minorRadius;
        if (b > a) {
            // OCCT keeps the larger radius as the major one.
            along = normal.Crossed(along);
            std::swap(a, b);
        }
        const gp_Elips ellipse(gp_Ax2(gp_Pnt(center.x, center.y, center.z), normal, along), a, b);
        return occt::wrap(BRepBuilderAPI_MakeEdge(ellipse).Edge());
    });
}

Shape makeSplineEdge(const Plane& plane,
                     const std::vector<std::pair<double, double>>& controlPoints, int degree) {
    const int n = static_cast<int>(controlPoints.size());
    if (n < 3 || degree < 1 || degree > n - 1) {
        throw std::invalid_argument("makeSplineEdge: three control points or more are needed, and "
                                    "a degree less than their count");
    }
    for (const auto& [u, v] : controlPoints) {
        requireFinite({u, v}, "makeSplineEdge");
    }
    return occt::guarded("make a spline", [&] {
        TColgp_Array1OfPnt poles(1, n);
        for (int i = 0; i < n; ++i) {
            const auto& [u, v] = controlPoints[static_cast<std::size_t>(i)];
            const Point p = toWorld(plane, u, v);
            poles.SetValue(i + 1, gp_Pnt(p.x, p.y, p.z));
        }
        // Clamped, with the inner knots evenly between 0 and 1.
        const int distinct = n - degree + 1;
        TColStd_Array1OfReal knots(1, distinct);
        TColStd_Array1OfInteger multiplicities(1, distinct);
        for (int i = 0; i < distinct; ++i) {
            knots.SetValue(i + 1, static_cast<double>(i) / (distinct - 1));
            multiplicities.SetValue(i + 1, i == 0 || i == distinct - 1 ? degree + 1 : 1);
        }
        const Handle(Geom_BSplineCurve) curve =
            new Geom_BSplineCurve(poles, knots, multiplicities, degree);
        return occt::wrap(BRepBuilderAPI_MakeEdge(curve).Edge());
    });
}

Shape makeArcEdge(const Plane& plane, double centerU, double centerV, double startU, double startV,
                  double endU, double endV) {
    requireFinite({centerU, centerV, startU, startV, endU, endV}, "makeArcEdge");
    const double radius = std::hypot(startU - centerU, startV - centerV);
    const double toEnd = std::hypot(endU - centerU, endV - centerV);
    if (radius <= 1e-7 || toEnd <= 1e-7) {
        throw std::invalid_argument("makeArcEdge: the ends must be away from the center");
    }
    return occt::guarded("make an arc", [&] {
        const gp_Circ circle(axesOf(plane, centerU, centerV), radius);
        const double from = std::atan2(startV - centerV, startU - centerU);
        double to = std::atan2(endV - centerV, endU - centerU);
        while (to <= from) {
            to += 2.0 * M_PI;
        }
        return occt::wrap(BRepBuilderAPI_MakeEdge(circle, from, to).Edge());
    });
}

Shape makeVertex(const Point& at) {
    requireFinite({at.x, at.y, at.z}, "makeVertex");
    return occt::guarded(
        "make a point", [&] { return occt::wrap(BRepBuilderAPI_MakeVertex(toOcct(at)).Vertex()); });
}

Shape makeCompound(const std::vector<Shape>& parts) {
    return occt::guarded("combine shapes", [&] {
        BRep_Builder builder;
        TopoDS_Compound compound;
        builder.MakeCompound(compound);
        bool any = false;
        for (const Shape& part : parts) {
            if (!part.isNull()) {
                builder.Add(compound, occt::unwrap(part));
                any = true;
            }
        }
        return any ? occt::wrap(compound) : Shape();
    });
}

std::optional<PlaneCurve> projectOnto(const Plane& plane, const Shape& shape) {
    if (shape.isNull()) {
        return std::nullopt;
    }
    return occt::guarded("projectOnto", [&]() -> std::optional<PlaneCurve> {
        const TopoDS_Shape& source = occt::unwrap(shape);
        const auto local = [&plane](const gp_Pnt& p) {
            return toLocal(plane, Point{p.X(), p.Y(), p.Z()});
        };
        PlaneCurve result;
        if (source.ShapeType() == TopAbs_VERTEX) {
            const PlaneCoordinates at = local(BRep_Tool::Pnt(TopoDS::Vertex(source)));
            result.kind = PlaneCurve::Kind::Point;
            result.u1 = at.u;
            result.v1 = at.v;
            return result;
        }
        if (source.ShapeType() != TopAbs_EDGE) {
            return std::nullopt;
        }
        const BRepAdaptor_Curve curve(TopoDS::Edge(source));
        const double first = curve.FirstParameter();
        const double last = curve.LastParameter();
        const PlaneCoordinates start = local(curve.Value(first));
        const PlaneCoordinates end = local(curve.Value(last));
        if (curve.GetType() == GeomAbs_Line) {
            result.u1 = start.u;
            result.v1 = start.v;
            result.u2 = end.u;
            result.v2 = end.v;
            // Seen end-on, a line is a point.
            result.kind = std::hypot(end.u - start.u, end.v - start.v) < 1e-7
                              ? PlaneCurve::Kind::Point
                              : PlaneCurve::Kind::Segment;
            return result;
        }
        if (curve.GetType() != GeomAbs_Circle) {
            return std::nullopt;
        }
        const gp_Circ circle = curve.Circle();
        const gp_Dir axis = circle.Axis().Direction();
        const double facing =
            axis.X() * plane.normal.x + axis.Y() * plane.normal.y + axis.Z() * plane.normal.z;
        if (std::abs(std::abs(facing) - 1.0) > 1e-9) {
            return std::nullopt;
        }
        const PlaneCoordinates center = local(circle.Location());
        result.u1 = center.u;
        result.v1 = center.v;
        result.radius = circle.Radius();
        if (std::abs(last - first) >= 2.0 * M_PI - 1e-9) {
            result.kind = PlaneCurve::Kind::Circle;
            return result;
        }
        // The edge runs counter-clockwise about the circle's own axis. Seen from the plane's
        // side that is counter-clockwise too only if the axis points the plane's way.
        result.kind = PlaneCurve::Kind::Arc;
        const PlaneCoordinates& from = facing > 0.0 ? start : end;
        const PlaneCoordinates& to = facing > 0.0 ? end : start;
        result.u2 = from.u;
        result.v2 = from.v;
        result.u3 = to.u;
        result.v3 = to.v;
        return result;
    });
}

} // namespace lcad::kernel
