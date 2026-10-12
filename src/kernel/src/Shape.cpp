#include "ShapeImpl.h"

#include <lcad/kernel/occt/ShapeAccess.h>

namespace lcad::kernel::occt {

Shape wrap(const TopoDS_Shape& shape) {
    if (shape.IsNull()) {
        return Shape();
    }
    return Shape(std::make_shared<const Shape::Impl>(Shape::Impl{shape}));
}

const TopoDS_Shape& unwrap(const Shape& shape) {
    static const TopoDS_Shape nullShape;
    return shape.isNull() ? nullShape : shape.impl()->shape;
}

} // namespace lcad::kernel::occt
