// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/SubShapes.h>

#include <lcad/kernel/occt/ShapeAccess.h>

#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <stdexcept>
#include <string>

namespace lcad::kernel {

namespace {

TopAbs_ShapeEnum toOcct(SubShapeKind kind) {
    switch (kind) {
    case SubShapeKind::Vertex:
        return TopAbs_VERTEX;
    case SubShapeKind::Edge:
        return TopAbs_EDGE;
    case SubShapeKind::Face:
        return TopAbs_FACE;
    }
    return TopAbs_SHAPE;
}

TopTools_IndexedMapOfShape mapOf(const Shape& shape, TopAbs_ShapeEnum type) {
    TopTools_IndexedMapOfShape map;
    if (!shape.isNull()) {
        TopExp::MapShapes(occt::unwrap(shape), type, map);
    }
    return map;
}

} // namespace

int subShapeCount(const Shape& shape, SubShapeKind kind) {
    return mapOf(shape, toOcct(kind)).Extent();
}

Shape subShape(const Shape& shape, SubShapeKind kind, int index) {
    const TopTools_IndexedMapOfShape map = mapOf(shape, toOcct(kind));
    if (index < 0 || index >= map.Extent()) {
        throw std::out_of_range("subShape: index " + std::to_string(index) + " is outside 0 to " +
                                std::to_string(map.Extent() - 1));
    }
    // OCCT numbers from 1.
    return occt::wrap(map.FindKey(index + 1));
}

namespace occt {

int subShapeIndex(const Shape& whole, const TopoDS_Shape& part) {
    if (whole.isNull() || part.IsNull()) {
        return -1;
    }
    const TopAbs_ShapeEnum type = part.ShapeType();
    if (type != TopAbs_VERTEX && type != TopAbs_EDGE && type != TopAbs_FACE) {
        return -1;
    }
    return mapOf(whole, type).FindIndex(part) - 1;
}

} // namespace occt

} // namespace lcad::kernel
