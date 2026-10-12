#pragma once

#include <lcad/kernel/Shape.h>

#include <optional>

namespace lcad::kernel {

/// A position in model space, in millimeters.
struct Point {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

/// An axis-aligned box in model space.
struct BoundingBox {
    Point min;
    Point max;
};

/// How many distinct sub-shapes of each kind a shape contains. An edge shared by two faces
/// counts once.
struct TopologyCount {
    int solids = 0;
    int shells = 0;
    int faces = 0;
    int wires = 0;
    int edges = 0;
    int vertices = 0;
};

// These functions measure the exact geometry, never a display mesh. All of them accept a null
// shape and report it as empty.

/// Enclosed volume of the shape's solids, in cubic millimeters.
[[nodiscard]] double volume(const Shape& shape);

/// Total area of the shape's faces, in square millimeters.
[[nodiscard]] double surfaceArea(const Shape& shape);

/// Centroid of the shape's solids, or nothing when the shape encloses no volume.
[[nodiscard]] std::optional<Point> centerOfMass(const Shape& shape);

/// The tightest axis-aligned box around the shape, or nothing when the shape is empty.
[[nodiscard]] std::optional<BoundingBox> boundingBox(const Shape& shape);

[[nodiscard]] TopologyCount countTopology(const Shape& shape);

/// Whether the shape passes the kernel's topology and geometry consistency checks. A null
/// shape is not valid.
[[nodiscard]] bool isValid(const Shape& shape);

} // namespace lcad::kernel
