// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Measure.h>

#include <lcad/kernel/occt/ShapeAccess.h>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Dir.hxx>

#include <cmath>
#include <stdexcept>

namespace lcad::kernel {

namespace {

GProp_GProps volumeProperties(const Shape& shape) {
    GProp_GProps properties;
    // Only closed shells enclose a volume; anything else would contribute a meaningless number.
    const bool onlyClosed = true;
    BRepGProp::VolumeProperties(occt::unwrap(shape), properties, onlyClosed);
    return properties;
}

int countOfType(const TopoDS_Shape& shape, TopAbs_ShapeEnum type) {
    TopTools_IndexedMapOfShape distinct;
    TopExp::MapShapes(shape, type, distinct);
    return distinct.Extent();
}

} // namespace

double volume(const Shape& shape) {
    if (shape.isNull()) {
        return 0.0;
    }
    return volumeProperties(shape).Mass();
}

double surfaceArea(const Shape& shape) {
    if (shape.isNull()) {
        return 0.0;
    }
    GProp_GProps properties;
    BRepGProp::SurfaceProperties(occt::unwrap(shape), properties);
    return properties.Mass();
}

double length(const Shape& shape) {
    if (shape.isNull()) {
        return 0.0;
    }
    GProp_GProps properties;
    // Count an edge once however many faces share it.
    const bool skipShared = true;
    BRepGProp::LinearProperties(occt::unwrap(shape), properties, skipShared);
    return properties.Mass();
}

std::optional<Point> centerOfMass(const Shape& shape) {
    if (shape.isNull()) {
        return std::nullopt;
    }
    const GProp_GProps properties = volumeProperties(shape);
    if (properties.Mass() <= 0.0) {
        return std::nullopt;
    }
    const gp_Pnt center = properties.CentreOfMass();
    return Point{center.X(), center.Y(), center.Z()};
}

std::optional<BoundingBox> boundingBox(const Shape& shape) {
    if (shape.isNull()) {
        return std::nullopt;
    }
    Bnd_Box box;
    // Measure the geometry itself: no display mesh, and no padding by the shape's tolerance.
    const bool useTriangulation = false;
    const bool useShapeTolerance = false;
    BRepBndLib::AddOptimal(occt::unwrap(shape), box, useTriangulation, useShapeTolerance);
    if (box.IsVoid()) {
        return std::nullopt;
    }
    BoundingBox result;
    box.Get(result.min.x, result.min.y, result.min.z, result.max.x, result.max.y, result.max.z);
    return result;
}

TopologyCount countTopology(const Shape& shape) {
    if (shape.isNull()) {
        return {};
    }
    const TopoDS_Shape& s = occt::unwrap(shape);
    TopologyCount count;
    count.solids = countOfType(s, TopAbs_SOLID);
    count.shells = countOfType(s, TopAbs_SHELL);
    count.faces = countOfType(s, TopAbs_FACE);
    count.wires = countOfType(s, TopAbs_WIRE);
    count.edges = countOfType(s, TopAbs_EDGE);
    count.vertices = countOfType(s, TopAbs_VERTEX);
    return count;
}

bool isValid(const Shape& shape) {
    if (shape.isNull()) {
        return false;
    }
    return BRepCheck_Analyzer(occt::unwrap(shape)).IsValid();
}

namespace {

// The direction a part stands for when angles are measured: a straight edge's direction, or a
// flat face's normal, and which of the two it is.
struct Bearing {
    gp_Dir direction;
    bool normal = false;
};

std::optional<Bearing> bearingOf(const TopoDS_Shape& part) {
    if (part.ShapeType() == TopAbs_EDGE) {
        const BRepAdaptor_Curve curve(TopoDS::Edge(part));
        if (curve.GetType() == GeomAbs_Line) {
            return Bearing{curve.Line().Direction(), false};
        }
    } else if (part.ShapeType() == TopAbs_FACE) {
        const BRepAdaptor_Surface surface(TopoDS::Face(part));
        if (surface.GetType() == GeomAbs_Plane) {
            return Bearing{surface.Plane().Axis().Direction(), true};
        }
    }
    return std::nullopt;
}

} // namespace

Measurement measureBetween(const Shape& first, const Shape& second) {
    if (first.isNull() || second.isNull()) {
        throw std::invalid_argument("measureBetween: a shape is null");
    }
    const TopoDS_Shape& a = occt::unwrap(first);
    const TopoDS_Shape& b = occt::unwrap(second);
    Measurement result;
    const BRepExtrema_DistShapeShape nearest(a, b);
    if (nearest.IsDone() && nearest.NbSolution() > 0) {
        result.distance = nearest.Value();
        const gp_Pnt p = nearest.PointOnShape1(1);
        const gp_Pnt q = nearest.PointOnShape2(1);
        result.from = {p.X(), p.Y(), p.Z()};
        result.to = {q.X(), q.Y(), q.Z()};
    }
    const auto one = bearingOf(a);
    const auto other = bearingOf(b);
    if (one.has_value() && other.has_value()) {
        // Lines and planes have no way round: the angle between them is at most a right angle.
        const double cosine = std::min(1.0, std::abs(one->direction.Dot(other->direction)));
        double degrees = std::acos(cosine) * 180.0 / 3.14159265358979323846;
        if (one->normal != other->normal) {
            // A line and a plane: measured from the plane, not from its normal.
            degrees = 90.0 - degrees;
        }
        result.angle = degrees;
    }
    return result;
}

std::optional<double> radiusOf(const Shape& part) {
    if (part.isNull()) {
        return std::nullopt;
    }
    const TopoDS_Shape& shape = occt::unwrap(part);
    if (shape.ShapeType() == TopAbs_EDGE) {
        const BRepAdaptor_Curve curve(TopoDS::Edge(shape));
        if (curve.GetType() == GeomAbs_Circle) {
            return curve.Circle().Radius();
        }
    } else if (shape.ShapeType() == TopAbs_FACE) {
        const BRepAdaptor_Surface surface(TopoDS::Face(shape));
        if (surface.GetType() == GeomAbs_Cylinder) {
            return surface.Cylinder().Radius();
        }
        if (surface.GetType() == GeomAbs_Sphere) {
            return surface.Sphere().Radius();
        }
    }
    return std::nullopt;
}

} // namespace lcad::kernel
