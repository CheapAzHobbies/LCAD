// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/kernel/Plane.h>
#include <lcad/kernel/Shape.h>

#include <optional>
#include <string>
#include <vector>

// The operations that make and change solids: pull an outline into a solid, turn it about an
// axis, combine two solids, round or bevel edges, hollow a solid out.
//
// Each of them works on a NamedShape and returns one. A failure that is in the nature of the
// operation (a radius too large for its edge, a cut that leaves nothing) is a kernel::Error
// whose reason can be shown to the user as it is.
namespace lcad::kernel {

/// A shape together with a name for each of its faces.
///
/// A name says where its face came from: which operation made it, and out of what. It stays the
/// same when the shape is built again from changed inputs, which the number of a face does not.
/// That is what lets a later operation keep hold of "the edge between the top and that side"
/// after the sketch underneath has been given another size.
struct NamedShape {
    Shape shape;
    /// One name per face, in the order subShape() numbers the faces. Unique within the shape.
    std::vector<std::string> faceNames;
};

/// Names every face `prefix` followed by its number. For a shape that has no history to name
/// its faces by: a primitive, an imported solid.
[[nodiscard]] NamedShape nameFaces(const Shape& shape, const std::string& prefix);

/// A name for every edge, in the order subShape() numbers the edges, made of the names of the
/// faces that meet in it. Where several edges lie between the same faces they are told apart
/// by a number, in the order of where they are.
[[nodiscard]] std::vector<std::string> edgeNames(const NamedShape& shape);

/// The number of the face or edge with that name, or -1 if the shape has none.
[[nodiscard]] int findFace(const NamedShape& shape, const std::string& name);
[[nodiscard]] int findEdge(const NamedShape& shape, const std::string& name);

/// One curve of a closed outline: an edge, and the name that goes into the name of whatever
/// face is made from it.
struct OutlineCurve {
    Shape edge;
    std::string name;
};

/// A flat region to make a solid from: the outline around it, and the outlines of the holes in
/// it. Each outline is a closed chain of curves, given in any order.
struct Region {
    std::vector<OutlineCurve> outer;
    std::vector<std::vector<OutlineCurve>> holes;
};

/// The region as a flat face, for showing it and picking it. Throws kernel::Error if the
/// curves do not close into outlines.
[[nodiscard]] Shape makeRegionFace(const Plane& plane, const Region& region);

/// Pulls regions of a plane into a solid that reaches from `from` to `to` along the plane's
/// normal, in millimeters from the plane; either may be negative, and they must differ. Regions
/// that touch or overlap become one solid.
///
/// Faces are named `tag`/side/<curve name>, `tag`/start/<name>, and `tag`/end/<name>, where the
/// last part is the name of a curve of the region's outline.
[[nodiscard]] NamedShape pull(const Plane& plane, const std::vector<Region>& regions, double from,
                              double to, const std::string& tag);

/// Turns regions of a plane about an axis that lies in the plane, through the points (u1, v1)
/// and (u2, v2), by `angle` radians: up to a full turn, counter-clockwise seen from the second
/// point looking toward the first. Faces are named as by pull(); a full turn has no start or
/// end.
[[nodiscard]] NamedShape revolve(const Plane& plane, const std::vector<Region>& regions, double u1,
                                 double v1, double u2, double v2, double angle,
                                 const std::string& tag);

/// How two solids are combined.
enum class Combination {
    Join,      ///< everything that is in either
    Cut,       ///< the body, less what the tool covers
    Intersect, ///< only what is in both
};

/// Combines a body with a tool. A face that survives keeps its name, also when it has been cut
/// to another outline; faces that end up lying in one plane become one face. Throws
/// kernel::Error if nothing is left.
[[nodiscard]] NamedShape combine(const NamedShape& body, const NamedShape& tool, Combination how);

/// Rounds the edges with the given numbers to `radius`. The new faces are named
/// `tag`/edge/<a number made from the edge's name>.
[[nodiscard]] NamedShape fillet(const NamedShape& body, const std::vector<int>& edges,
                                double radius, const std::string& tag);

/// Bevels the edges with the given numbers by `distance` on each side. The new faces are named
/// as by fillet().
[[nodiscard]] NamedShape chamfer(const NamedShape& body, const std::vector<int>& edges,
                                 double distance, const std::string& tag);

/// Hollows a solid out, leaving walls of `thickness`, open at the faces with the given numbers.
/// The inside of a face is named `tag`/inner/<a number made from the face's name>.
[[nodiscard]] NamedShape shell(const NamedShape& body, const std::vector<int>& openFaces,
                               double thickness, const std::string& tag);

/// A line in space: a point on it, and its direction, of unit length.
struct Axis {
    Point origin;
    Vector direction{0.0, 0.0, 1.0};
};

/// The line a part of a shape stands for: a straight edge's own line; the axis of a circular
/// edge, or of a cylindrical, conical, spherical, or toroidal face; the normal of a flat face,
/// through its middle. Nothing for anything else, and for a null shape.
[[nodiscard]] std::optional<Axis> axisOf(const Shape& part);

// Copies, for mirroring and patterns. Each face of a copy keeps the name of the face it was
// copied from, with `prefix` and a slash put in front, so that copies are told apart from the
// original and from each other.

/// A copy of `shape` moved by `by`, in millimeters.
[[nodiscard]] NamedShape moved(const NamedShape& shape, const Vector& by,
                               const std::string& prefix);
/// A copy of `shape` turned about `axis` by `angle` radians, counter-clockwise as seen looking
/// against the axis's direction.
[[nodiscard]] NamedShape turned(const NamedShape& shape, const Axis& axis, double angle,
                                const std::string& prefix);
/// The mirror image of `shape` in `plane`.
[[nodiscard]] NamedShape mirrored(const NamedShape& shape, const Plane& plane,
                                  const std::string& prefix);

} // namespace lcad::kernel
