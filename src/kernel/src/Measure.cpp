#include <lcad/kernel/Measure.h>

#include <lcad/kernel/occt/ShapeAccess.h>

#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

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

} // namespace lcad::kernel
