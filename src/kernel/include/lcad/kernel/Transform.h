// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/kernel/Shape.h>

namespace lcad::kernel {

/// `shape` moved by the given distances along X, Y, and Z, in millimeters. The original is not
/// changed, and the result shares its geometry. A null shape stays null.
///
/// Throws std::invalid_argument when a distance is not finite.
[[nodiscard]] Shape translated(const Shape& shape, double dx, double dy, double dz);

} // namespace lcad::kernel
