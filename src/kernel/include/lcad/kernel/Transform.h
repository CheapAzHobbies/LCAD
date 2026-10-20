// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/kernel/Plane.h>
#include <lcad/kernel/Shape.h>

#include <optional>

// Where things are: moving shapes, the placement of one part within another, and the frames
// that parts of a shape give, which joints line components up by.
namespace lcad::kernel {

/// `shape` moved by the given distances along X, Y, and Z, in millimeters. The original is not
/// changed, and the result shares its geometry. A null shape stays null.
///
/// Throws std::invalid_argument when a distance is not finite.
[[nodiscard]] Shape translated(const Shape& shape, double dx, double dy, double dz);

/// Where a part sits within what it is part of: its own origin and its own X, Y, and Z, given
/// in the coordinates of that larger whole. The three directions are of unit length, square to
/// each other, and right-handed, so a placement turns and moves without stretching or
/// mirroring. The default leaves things where they are.
///
/// A placement is also a frame: a point with three directions, such as the one a joint lines
/// two components up by.
struct Placement {
    Point origin;
    Vector xAxis{1.0, 0.0, 0.0};
    Vector yAxis{0.0, 1.0, 0.0};
    Vector zAxis{0.0, 0.0, 1.0};
};

/// The placement that `inner` becomes inside `outer`: first `inner`, then `outer`. A part
/// placed by `inner` in an assembly placed by `outer` is placed by `outer * inner`.
[[nodiscard]] Placement operator*(const Placement& outer, const Placement& inner);
/// The placement that undoes this one.
[[nodiscard]] Placement inverse(const Placement& placement);
/// Where a point, a direction, a plane, or a line of the part goes.
[[nodiscard]] Point apply(const Placement& placement, const Point& point);
[[nodiscard]] Vector apply(const Placement& placement, const Vector& direction);
[[nodiscard]] Plane apply(const Placement& placement, const Plane& plane);
[[nodiscard]] Axis apply(const Placement& placement, const Axis& axis);

/// A move by `by`, without turning.
[[nodiscard]] Placement translation(const Vector& by);
/// A turn by `angle` radians about a line, counter-clockwise as seen looking against the line's
/// direction. The direction need not be of unit length but must not be zero.
[[nodiscard]] Placement rotation(const Axis& about, double angle);
/// The frame of a plane: its origin, its xDirection as X, and its normal as Z.
[[nodiscard]] Placement frameOf(const Plane& plane);
/// A frame at `origin` with `z` as its Z and an X square to it: along the model's X as far as
/// `z` allows, or along its Y where `z` is the model's X. `z` must not be zero.
[[nodiscard]] Placement frameAt(const Point& origin, const Vector& z);

/// Whether two placements are the same to within `tolerance`, in millimeters and in the
/// length of the difference between their unit directions.
[[nodiscard]] bool samePlacement(const Placement& a, const Placement& b, double tolerance = 1e-9);
/// Whether the directions are of unit length, square, right-handed, and every number finite.
[[nodiscard]] bool isRigid(const Placement& placement);
/// The placement with its directions made exactly square and of unit length again, keeping Z
/// and the plane of Z and X: undoes the drift of many placements put together. Throws
/// std::invalid_argument if Z is zero or X lies along it.
[[nodiscard]] Placement normalized(const Placement& placement);

/// `shape` placed by `placement`. The result shares the original's geometry; a null shape
/// stays null.
[[nodiscard]] Shape placed(const Shape& shape, const Placement& placement);

/// Where on a part of a shape a frame is taken, where there is a choice.
enum class FrameSnap {
    /// The middle: of a straight edge, of the stretch of axis a round face covers.
    Center,
    /// The start of a straight edge, or the end of a round face's axis the axis points away
    /// from.
    Start,
    /// The other end.
    End,
};

/// The frame a part of a shape gives, which a joint lines up by. Z is:
/// - for a flat face, its normal out of the solid, at the face's center of area;
/// - for a round face (cylinder, cone, sphere, torus), its axis, at the middle of the stretch of
///   axis the face covers (or at an end, by `snap`), or a sphere's center;
/// - for a circular edge, the axis of its circle, at its center;
/// - for a straight edge, its direction, at its middle, start, or end;
/// - for a vertex, the model's Z, at the vertex;
/// - for another face, its normal at the middle of its parameters.
/// X is chosen as by frameAt(). Nothing for anything else, and for a null shape.
[[nodiscard]] std::optional<Placement> frameOn(const Shape& part,
                                               FrameSnap snap = FrameSnap::Center);

} // namespace lcad::kernel
