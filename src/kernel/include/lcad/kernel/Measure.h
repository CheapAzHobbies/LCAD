// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/kernel/Shape.h>

#include <array>
#include <optional>
#include <vector>

namespace lcad::kernel {

/// A position in model space, in millimeters.
struct Point {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

/// A direction in model space.
struct Vector {
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

/// Total length of the shape's edges, in millimeters. An edge shared by two faces counts once.
[[nodiscard]] double length(const Shape& shape);

/// Centroid of the shape's solids, or nothing when the shape encloses no volume.
[[nodiscard]] std::optional<Point> centerOfMass(const Shape& shape);

/// The tightest axis-aligned box around the shape, or nothing when the shape is empty.
[[nodiscard]] std::optional<BoundingBox> boundingBox(const Shape& shape);

[[nodiscard]] TopologyCount countTopology(const Shape& shape);

/// Whether the shape passes the kernel's topology and geometry consistency checks. A null
/// shape is not valid.
[[nodiscard]] bool isValid(const Shape& shape);

/// How two shapes stand to each other.
struct Measurement {
    /// The shortest distance between them, in millimeters. Zero where they touch or overlap.
    double distance = 0.0;
    /// Where on each the shortest distance is.
    Point from;
    Point to;
    /// For two flat faces, two straight edges, or one of each: the angle between them, in
    /// degrees, from 0 (parallel) to 90 (square).
    std::optional<double> angle;
};

/// Measures between two shapes: vertices, edges, faces, or whole bodies. Throws
/// std::invalid_argument for a null shape.
[[nodiscard]] Measurement measureBetween(const Shape& first, const Shape& second);

/// The radius of a circular edge, or of a cylindrical or spherical face. Nothing for others.
[[nodiscard]] std::optional<double> radiusOf(const Shape& part);

/// What kind of face, edge, or vertex a part of a shape is.
enum class PartKind {
    Flat,
    Cylindrical,
    Conical,
    Spherical,
    Toroidal,
    /// Another kind of face.
    Curved,
    Straight,
    Circular,
    /// Another kind of edge.
    Bent,
    Point,
    /// A null shape, or one that is not a face, an edge, or a vertex.
    Other,
};

[[nodiscard]] PartKind partKind(const Shape& part);

/// How much a solid weighs, where its weight is centered, and how it resists being turned.
struct MassProperties {
    /// Cubic millimeters.
    double volume = 0.0;
    /// Grams.
    double mass = 0.0;
    Point center;
    /// The inertia tensor about `center`, along the model's X, Y, and Z, in gram square
    /// millimeters: the moments about the three axes on the diagonal, and the products of
    /// inertia, with their sign turned, off it. Symmetric.
    std::array<std::array<double, 3>, 3> inertia{};
};

/// The mass properties of a shape's solids at `density` grams per cubic centimeter. Nothing if
/// the shape encloses no volume. Throws std::invalid_argument for a density that is not a
/// positive number.
[[nodiscard]] std::optional<MassProperties> massProperties(const Shape& shape, double density);

/// Several masses taken together: their masses and volumes added, their center the mean of
/// theirs weighted by mass, their inertia moved to that center and added. Empty for none.
[[nodiscard]] MassProperties combined(const std::vector<MassProperties>& parts);

/// The axes a solid turns about most and least easily, and its moments of inertia about them.
struct PrincipalInertia {
    /// Smallest first, in gram square millimeters.
    std::array<double, 3> moments{};
    /// Of unit length, square to each other, one for each moment.
    std::array<Vector, 3> axes{};
};

[[nodiscard]] PrincipalInertia principalInertia(const MassProperties& properties);

/// Where two solids share space: the solid they have in common, and its volume.
struct Overlap {
    Shape shape;
    double volume = 0.0;
};

/// The space two shapes' solids share, or nothing when they share none: solids that only touch,
/// face to face or along an edge, do not overlap. Throws kernel::Error if it cannot be worked
/// out.
[[nodiscard]] std::optional<Overlap> overlapOf(const Shape& first, const Shape& second);

} // namespace lcad::kernel
