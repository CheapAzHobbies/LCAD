// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Tolerances for geometry assertions. Every comparison of a measured value names one of these;
// see docs/TESTING.md for how to choose.
namespace lcad::test {

/// Lengths and coordinates, in millimeters. One order of magnitude looser than the kernel's own
/// modeling tolerance of 1e-7 mm.
inline constexpr double lengthTolerance = 1e-6;

/// Relative tolerance for areas and volumes of shapes bounded only by planes, where the kernel
/// integrates exactly up to floating-point rounding.
inline constexpr double exactPropertyTolerance = 1e-9;

/// Relative tolerance for areas and volumes of solids bounded by cylinders, spheres, cones, and
/// tori. The kernel integrates these analytic surfaces to rounding as well (errors of about
/// 1e-16 were measured with OCCT 7.6). Kept apart from the constant above because spline
/// surfaces, when they arrive, will need a looser one and must not loosen this.
inline constexpr double curvedPropertyTolerance = 1e-9;

} // namespace lcad::test
