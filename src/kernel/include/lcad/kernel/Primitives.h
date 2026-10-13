// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/kernel/Shape.h>

// Primitive solids. Sizes are in millimeters. Every function throws std::invalid_argument when
// a size is not a finite number that gives a non-degenerate solid; "a usable length" below means
// finite and greater than `minimumLength`.
namespace lcad::kernel {

/// The shortest length the kernel treats as non-degenerate, in millimeters.
inline constexpr double minimumLength = 1e-7;

/// A solid box with one corner at the origin, extending along +X, +Y, and +Z.
[[nodiscard]] Shape makeBox(double sizeX, double sizeY, double sizeZ);

/// A solid cylinder standing on the XY plane: its base circle is centered on the origin and its
/// axis runs along +Z.
[[nodiscard]] Shape makeCylinder(double radius, double height);

/// A solid sphere centered on the origin.
[[nodiscard]] Shape makeSphere(double radius);

/// A solid cone, or a truncated one, standing on the XY plane with its axis along +Z.
/// `baseRadius` is the radius at Z = 0 and `topRadius` the radius at Z = `height`. One of the
/// two may be zero, which gives a point at that end. They must differ: equal radii describe a
/// cylinder.
[[nodiscard]] Shape makeCone(double baseRadius, double topRadius, double height);

/// A solid torus (ring) centered on the origin, lying in the XY plane. `majorRadius` is the
/// distance from the center to the middle of the tube, and `minorRadius` is the radius of the
/// tube, which must be the smaller of the two so that the ring has a hole.
[[nodiscard]] Shape makeTorus(double majorRadius, double minorRadius);

} // namespace lcad::kernel
