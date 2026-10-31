// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/kernel/Projection.h>

#include <vector>

// Flat areas, for working out toolpaths: Clipper2 behind a small interface.
namespace lcad::cam {

using Loop = std::vector<kernel::Point2D>;

/// A flat area: closed loops. An outline runs counter-clockwise, and a hole clockwise.
using Area = std::vector<Loop>;

/// A region as an area.
[[nodiscard]] Area areaOf(const kernel::FlatRegion& region);

/// The area grown by `distance`, or shrunk by a negative one, with round corners where it grows.
[[nodiscard]] Area offset(const Area& area, double distance);

/// How large the area is, in square millimeters.
[[nodiscard]] double sizeOf(const Area& area);

/// What of `area` is left once `removed` is taken away.
[[nodiscard]] Area difference(const Area& area, const Area& removed);

/// The area that a tool of a radius sweeps along a path.
[[nodiscard]] Area swept(const std::vector<Loop>& paths, bool closed, double radius);

/// Whether a straight line from `from` to `to` stays inside `area`.
[[nodiscard]] bool lineInside(const Area& area, const kernel::Point2D& from,
                              const kernel::Point2D& to);

/// Whether a point is inside a loop's outline, or on it.
[[nodiscard]] bool pointInside(const Area& area, const kernel::Point2D& point);

} // namespace lcad::cam
