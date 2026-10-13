// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Shape.h>

#include <optional>
#include <vector>

namespace lcad::kernel {

/// A direction in model space.
struct Vector {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

/// A plane with a coordinate system on it: where a sketch lives in space. A point of the plane
/// is given by two numbers, u along `xDirection` and v along yDirection(), from `origin`.
struct Plane {
    Point origin;
    /// Unit length. The side of the plane a sketch is looked at from.
    Vector normal{0.0, 0.0, 1.0};
    /// Unit length, and square to the normal.
    Vector xDirection{1.0, 0.0, 0.0};
};

/// A plane through `origin`. `normal` and `xDirection` need not be of unit length; they must be
/// finite, not zero, and square to each other to within a millionth. Throws
/// std::invalid_argument otherwise.
[[nodiscard]] Plane makePlane(const Point& origin, const Vector& normal, const Vector& xDirection);

// The three planes through the origin. Each is set up so that its sketch is seen the right way
// round from the matching standard view: XY from the top, XZ from the front, YZ from the right.
[[nodiscard]] Plane planeXY(); ///< u along X, v along Y, seen from +Z
[[nodiscard]] Plane planeXZ(); ///< u along X, v along Z, seen from -Y
[[nodiscard]] Plane planeYZ(); ///< u along Y, v along Z, seen from +X

/// The plane's second in-plane direction: its normal crossed with its xDirection.
[[nodiscard]] Vector yDirection(const Plane& plane);

/// The point of model space at (u, v) on the plane.
[[nodiscard]] Point toWorld(const Plane& plane, double u, double v);

/// A point of model space in the plane's own coordinates: u and v on the plane, and w its
/// height above the plane along the normal.
struct PlaneCoordinates {
    double u = 0.0;
    double v = 0.0;
    double w = 0.0;
};
[[nodiscard]] PlaneCoordinates toLocal(const Plane& plane, const Point& point);

/// The plane a flat face lies in, with its normal pointing out of the solid, its origin where
/// the model's origin drops onto it, and its xDirection along the model's X as far as the plane
/// allows. Nothing if `face` is not one flat face.
[[nodiscard]] std::optional<Plane> planeOfFace(const Shape& face);

/// What an edge or a vertex becomes when it is dropped straight onto a plane, in the plane's
/// own coordinates.
struct PlaneCurve {
    enum class Kind {
        Point,   ///< at (u1, v1)
        Segment, ///< from (u1, v1) to (u2, v2)
        Circle,  ///< around (u1, v1) with `radius`
        Arc,     ///< around (u1, v1), from (u2, v2) counter-clockwise to (u3, v3)
    };
    Kind kind = Kind::Point;
    double u1 = 0.0;
    double v1 = 0.0;
    double u2 = 0.0;
    double v2 = 0.0;
    double u3 = 0.0;
    double v3 = 0.0;
    double radius = 0.0;
};

/// Drops a vertex, a straight edge, or a circular edge whose circle is parallel to the plane
/// onto the plane, along the plane's normal. A straight edge that is square to the plane
/// becomes a point. Nothing for any other shape: a circle seen at a slant is an ellipse, which
/// this cannot describe.
[[nodiscard]] std::optional<PlaneCurve> projectOnto(const Plane& plane, const Shape& shape);

// Curves, as shapes. These are what a sketch's geometry becomes in model space.

/// A straight edge from `start` to `end`. Throws std::invalid_argument if they coincide.
[[nodiscard]] Shape makeSegment(const Point& start, const Point& end);
/// A full circle in the plane, centered at (centerU, centerV).
[[nodiscard]] Shape makeCircleEdge(const Plane& plane, double centerU, double centerV,
                                   double radius);
/// An arc in the plane around (centerU, centerV), from (startU, startV) counter-clockwise, as
/// seen from the side the normal points to, to the direction of (endU, endV).
[[nodiscard]] Shape makeArcEdge(const Plane& plane, double centerU, double centerV, double startU,
                                double startV, double endU, double endV);
/// A single point as a shape.
[[nodiscard]] Shape makeVertex(const Point& at);
/// Several shapes as one. Null shapes are left out; an empty list gives a null shape.
[[nodiscard]] Shape makeCompound(const std::vector<Shape>& parts);

} // namespace lcad::kernel
