// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/kernel/Shape.h>

namespace lcad::kernel {

/// The kinds of sub-shape a user can pick.
enum class SubShapeKind { Vertex, Edge, Face };

// Sub-shapes are numbered from 0 in the order the kernel visits them. An edge shared by two
// faces has one number. The numbering holds for one Shape value only: it is not a reference
// that survives a modeling operation. Stable references are a later design (see
// docs/ARCHITECTURE.md).

/// How many distinct sub-shapes of `kind` the shape has.
[[nodiscard]] int subShapeCount(const Shape& shape, SubShapeKind kind);

/// The sub-shape with the given number. Throws std::out_of_range when there is none.
[[nodiscard]] Shape subShape(const Shape& shape, SubShapeKind kind, int index);

} // namespace lcad::kernel
