#pragma once

// Tolerances for geometry assertions. Every comparison of a measured value names one of these;
// see docs/TESTING.md for how to choose.
namespace lcad::test {

/// Lengths and coordinates, in millimetres. One order of magnitude looser than the kernel's own
/// modelling tolerance of 1e-7 mm.
inline constexpr double lengthTolerance = 1e-6;

/// Relative tolerance for areas and volumes of shapes bounded only by planes, where the kernel
/// integrates exactly up to floating-point rounding.
inline constexpr double exactPropertyTolerance = 1e-9;

} // namespace lcad::test
