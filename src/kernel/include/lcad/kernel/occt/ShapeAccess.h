#pragma once

#include <lcad/kernel/Shape.h>

#include <TopoDS_Shape.hxx>

namespace lcad::kernel::occt {

/// Wraps an OCCT shape. A null TopoDS_Shape gives a null Shape.
[[nodiscard]] Shape wrap(const TopoDS_Shape& shape);

/// The OCCT shape behind `shape`, or a null TopoDS_Shape when `shape` is null.
[[nodiscard]] const TopoDS_Shape& unwrap(const Shape& shape);

} // namespace lcad::kernel::occt
