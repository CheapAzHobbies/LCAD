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
///
/// `taper`, in radians, tilts the sides: a positive taper makes the solid narrower the farther
/// it goes from the plane, a negative one wider. A taper needs a pull to one side: `from` or `to`
/// must be zero. Throws kernel::Error if the taper is too steep for the outline.
[[nodiscard]] NamedShape pull(const Plane& plane, const std::vector<Region>& regions, double from,
                              double to, const std::string& tag, double taper = 0.0);

/// Pulls regions of a plane along its normal, or against it, up to `limit`: the solid ends where
/// it meets that plane, which need not be parallel to the regions'. The end is named as by
/// pull(). Throws kernel::Error if the limit runs along the pull or passes through the regions.
[[nodiscard]] NamedShape pullTo(const Plane& plane, const std::vector<Region>& regions,
                                const Plane& limit, const std::string& tag);

/// Turns regions of a plane about an axis that lies in the plane, through the points (u1, v1)
/// and (u2, v2), by `angle` radians: up to a full turn, counter-clockwise seen from the second
/// point looking toward the first. Faces are named as by pull(); a full turn has no start or
/// end.
[[nodiscard]] NamedShape revolve(const Plane& plane, const std::vector<Region>& regions, double u1,
                                 double v1, double u2, double v2, double angle,
                                 const std::string& tag);

struct Axis;

/// Turns regions of a plane about an axis anywhere in space, by `angle` radians,
/// counter-clockwise seen looking against the axis's direction. Faces are named as by pull().
[[nodiscard]] NamedShape revolve(const Plane& plane, const std::vector<Region>& regions,
                                 const Axis& axis, double angle, const std::string& tag);

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

// Direct edits: changes to the faces of a solid as it is, whatever made it, such as a solid
// read from a STEP file. A face that is changed keeps its name.

/// Moves faces along their outward normals by `distance` in millimeters, the faces around them
/// stretching or shrinking to meet them: a positive distance adds material, a negative one
/// takes it away. A flat face moves parallel to itself; a round one, such as the wall of a
/// hole, grows or shrinks. Throws kernel::Error if the faces around cannot meet them there.
[[nodiscard]] NamedShape offsetFaces(const NamedShape& body, const std::vector<int>& faces,
                                     double distance, const std::string& tag);

/// Takes faces out of a solid, the faces around them extending to close the gap: a fillet, a
/// hole, a boss goes as if it had never been made. Throws kernel::Error if the faces around
/// cannot close the gap.
[[nodiscard]] NamedShape removeFaces(const NamedShape& body, const std::vector<int>& faces,
                                     const std::string& tag);

/// Moves faces by `by`, in millimeters, the faces around them following. Flat faces move along
/// their normals (a flat face moved within its own plane does not change). Faces that are not
/// all flat move as one piece: the material they bound is taken out, as by removeFaces(), and
/// put back moved, so that a hole or a boss can be moved across the face it is in. Throws
/// kernel::Error if that cannot be done.
[[nodiscard]] NamedShape moveFaces(const NamedShape& body, const std::vector<int>& faces,
                                   const Vector& by, const std::string& tag);

/// A solid with its defects put right where that can be done: loose faces sewn together,
/// faces turned to face out, edges and tolerances fixed. A face that is fixed keeps its name.
/// Throws kernel::Error if no valid solid can be made of it.
[[nodiscard]] NamedShape repaired(const NamedShape& body, const std::string& tag);

/// The kinds of hole.
enum class HoleKind {
    Simple,
    /// Wider at the top, with a flat step: for the head of a cap screw.
    Counterbore,
    /// Wider at the top, in a cone: for the head of a flat screw.
    Countersink,
};

/// What a hole looks like. Lengths in millimeters, angles in degrees.
struct HoleShape {
    HoleKind kind = HoleKind::Simple;
    double diameter = 5.0;
    /// How deep the full diameter goes. A hole through is as deep as it needs to be instead.
    double depth = 10.0;
    /// The angle of the point a drill leaves at the bottom of a hole that is not through. 180
    /// for a flat bottom.
    double tipAngle = 118.0;
    /// Counterbore and countersink: the diameter at the top.
    double headDiameter = 10.0;
    /// Counterbore: how deep the wider part goes.
    double headDepth = 3.0;
    /// Countersink: the angle of its cone.
    double countersinkAngle = 90.0;
};

/// The solid a hole takes away, starting at `start` and going along `direction`, which must
/// not be zero. `through`, if more than zero, makes it that deep with a flat bottom instead of
/// `shape.depth` and its point. The faces are named `tag`/side/<part>@`name`, where part is
/// top, bore, tip, floor, counterbore, shoulder, or countersink. Throws std::invalid_argument
/// for sizes that describe no hole.
[[nodiscard]] NamedShape holeTool(const Point& start, const Vector& direction,
                                  const HoleShape& shape, double through, const std::string& tag,
                                  const std::string& name);

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
