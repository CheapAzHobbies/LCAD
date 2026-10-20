// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Measure.h>

#include <lcad/kernel/occt/Guard.h>
#include <lcad/kernel/occt/ShapeAccess.h>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
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
#include <gp_Mat.hxx>

#include <algorithm>
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

PartKind partKind(const Shape& part) {
    if (part.isNull()) {
        return PartKind::Other;
    }
    const TopoDS_Shape& shape = occt::unwrap(part);
    switch (shape.ShapeType()) {
    case TopAbs_VERTEX:
        return PartKind::Point;
    case TopAbs_EDGE: {
        const BRepAdaptor_Curve curve(TopoDS::Edge(shape));
        return curve.GetType() == GeomAbs_Line     ? PartKind::Straight
               : curve.GetType() == GeomAbs_Circle ? PartKind::Circular
                                                   : PartKind::Bent;
    }
    case TopAbs_FACE: {
        const BRepAdaptor_Surface surface(TopoDS::Face(shape));
        switch (surface.GetType()) {
        case GeomAbs_Plane:
            return PartKind::Flat;
        case GeomAbs_Cylinder:
            return PartKind::Cylindrical;
        case GeomAbs_Cone:
            return PartKind::Conical;
        case GeomAbs_Sphere:
            return PartKind::Spherical;
        case GeomAbs_Torus:
            return PartKind::Toroidal;
        default:
            return PartKind::Curved;
        }
    }
    default:
        return PartKind::Other;
    }
}

std::optional<MassProperties> massProperties(const Shape& shape, double density) {
    if (!std::isfinite(density) || density <= 0.0) {
        throw std::invalid_argument("massProperties: the density has to be more than zero");
    }
    if (shape.isNull()) {
        return std::nullopt;
    }
    const GProp_GProps properties = volumeProperties(shape);
    if (properties.Mass() <= 0.0) {
        return std::nullopt;
    }
    // Grams per cubic centimeter are thousandths of a gram per cubic millimeter.
    const double perCubicMillimeter = density * 1e-3;
    MassProperties result;
    result.volume = properties.Mass();
    result.mass = result.volume * perCubicMillimeter;
    const gp_Pnt center = properties.CentreOfMass();
    result.center = {center.X(), center.Y(), center.Z()};
    // OCCT gives the tensor about the center of mass, along the model's axes, for a density of
    // one.
    const gp_Mat matrix = properties.MatrixOfInertia();
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            result.inertia[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)] =
                matrix.Value(row + 1, column + 1) * perCubicMillimeter;
        }
    }
    return result;
}

MassProperties combined(const std::vector<MassProperties>& parts) {
    MassProperties total;
    for (const MassProperties& part : parts) {
        total.volume += part.volume;
        total.mass += part.mass;
        total.center.x += part.center.x * part.mass;
        total.center.y += part.center.y * part.mass;
        total.center.z += part.center.z * part.mass;
    }
    if (total.mass <= 0.0) {
        return total;
    }
    total.center = {total.center.x / total.mass, total.center.y / total.mass,
                    total.center.z / total.mass};
    for (const MassProperties& part : parts) {
        // Each part's inertia moved from its own center to the common one: m (|d|^2 I - d d^T).
        const std::array<double, 3> d{part.center.x - total.center.x,
                                      part.center.y - total.center.y,
                                      part.center.z - total.center.z};
        const double squared = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t column = 0; column < 3; ++column) {
                const double shift =
                    part.mass * ((row == column ? squared : 0.0) - d[row] * d[column]);
                total.inertia[row][column] += part.inertia[row][column] + shift;
            }
        }
    }
    return total;
}

PrincipalInertia principalInertia(const MassProperties& properties) {
    // Jacobi's method: turn the symmetric tensor until nothing is left off its diagonal. The
    // turns taken together are the axes.
    std::array<std::array<double, 3>, 3> a = properties.inertia;
    std::array<std::array<double, 3>, 3> v{{{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}}};
    for (int sweep = 0; sweep < 50; ++sweep) {
        const double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
        const double scale = a[0][0] * a[0][0] + a[1][1] * a[1][1] + a[2][2] * a[2][2];
        if (off <= 1e-30 * std::max(scale, 1e-300)) {
            break;
        }
        for (std::size_t p = 0; p < 2; ++p) {
            for (std::size_t q = p + 1; q < 3; ++q) {
                if (a[p][q] == 0.0) {
                    continue;
                }
                const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const double t = (theta >= 0.0 ? 1.0 : -1.0) /
                                 (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;
                for (std::size_t k = 0; k < 3; ++k) {
                    const double akp = a[k][p];
                    const double akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (std::size_t k = 0; k < 3; ++k) {
                    const double apk = a[p][k];
                    const double aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (std::size_t k = 0; k < 3; ++k) {
                    const double vkp = v[k][p];
                    const double vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq;
                    v[k][q] = s * vkp + c * vkq;
                }
            }
        }
    }
    std::array<std::size_t, 3> order{0, 1, 2};
    std::sort(order.begin(), order.end(),
              [&a](std::size_t i, std::size_t j) { return a[i][i] < a[j][j]; });
    PrincipalInertia result;
    for (std::size_t i = 0; i < 3; ++i) {
        const std::size_t column = order[i];
        result.moments[i] = a[column][column];
        result.axes[i] = {v[0][column], v[1][column], v[2][column]};
    }
    return result;
}

std::optional<Overlap> overlapOf(const Shape& first, const Shape& second) {
    if (first.isNull() || second.isNull()) {
        return std::nullopt;
    }
    const TopoDS_Shape& a = occt::unwrap(first);
    const TopoDS_Shape& b = occt::unwrap(second);
    if (countOfType(a, TopAbs_SOLID) == 0 || countOfType(b, TopAbs_SOLID) == 0) {
        return std::nullopt;
    }
    // Most pairs of parts are nowhere near each other: their boxes tell so cheaply.
    Bnd_Box boxA;
    Bnd_Box boxB;
    BRepBndLib::Add(a, boxA);
    BRepBndLib::Add(b, boxB);
    if (boxA.IsOut(boxB)) {
        return std::nullopt;
    }
    return occt::guarded("find where two solids overlap", [&]() -> std::optional<Overlap> {
        BRepAlgoAPI_Common common(a, b);
        if (!common.IsDone()) {
            throw Error("find where two solids overlap",
                        "the solids could not be intersected with each other");
        }
        const TopoDS_Shape& shared = common.Shape();
        if (shared.IsNull() || countOfType(shared, TopAbs_SOLID) == 0) {
            return std::nullopt;
        }
        Overlap overlap{occt::wrap(shared), 0.0};
        overlap.volume = volume(overlap.shape);
        // Faces that touch can leave a sliver no thicker than their tolerance.
        const double smallest = std::min(volume(first), volume(second));
        if (overlap.volume <= std::max(1e-6, 1e-9 * smallest)) {
            return std::nullopt;
        }
        return overlap;
    });
}

} // namespace lcad::kernel
