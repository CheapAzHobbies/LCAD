// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/kernel/Shape.h>

#include <TopoDS_Shape.hxx>

namespace lcad::kernel::occt {

/// Wraps an OCCT shape. A null TopoDS_Shape gives a null Shape.
[[nodiscard]] Shape wrap(const TopoDS_Shape& shape);

/// The OCCT shape behind `shape`, or a null TopoDS_Shape when `shape` is null.
[[nodiscard]] const TopoDS_Shape& unwrap(const Shape& shape);

/// The number kernel::subShape() gives to `part` within `whole`, or -1 when `part` is not a
/// vertex, edge, or face of it. Orientation is ignored.
[[nodiscard]] int subShapeIndex(const Shape& whole, const TopoDS_Shape& part);

} // namespace lcad::kernel::occt
