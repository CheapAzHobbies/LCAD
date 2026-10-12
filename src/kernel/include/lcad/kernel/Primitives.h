#pragma once

#include <lcad/kernel/Shape.h>

namespace lcad::kernel {

/// The shortest length the kernel treats as non-degenerate, in millimetres.
inline constexpr double minimumLength = 1e-7;

/// A solid box with one corner at the origin, extending along +X, +Y, and +Z. Sizes are in
/// millimetres.
///
/// Throws std::invalid_argument when a size is not a finite number greater than
/// `minimumLength`.
[[nodiscard]] Shape makeBox(double sizeX, double sizeY, double sizeZ);

} // namespace lcad::kernel
