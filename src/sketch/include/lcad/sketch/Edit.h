// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/sketch/Sketch.h>

#include <optional>
#include <vector>

// What the sketch tools do, as plain functions on a Sketch: find geometry at a position, build
// shapes out of several curves, work out which constraint a selection calls for, and modify
// curves (trim, extend, offset, mirror). The interface decides when to call them; nothing here
// knows about pointers, pixels, or undo.
namespace lcad::sketch {

// Finding geometry by position

/// How far a position is from an entity: from a point, from a line between its ends, from a
/// circle's rim, from an arc between its ends.
[[nodiscard]] double distanceTo(const Sketch& sketch, EntityId entity, Point2 position);

/// The place on a curve that is nearest to a position.
[[nodiscard]] Point2 nearestOn(const Sketch& sketch, EntityId curve, Point2 position);

/// The entities within `tolerance` of a position: points first, then curves, each group
/// nearest first. A point wins over the curve it lies on, as a user aiming at an end expects.
[[nodiscard]] std::vector<EntityId> entitiesAt(const Sketch& sketch, Point2 position,
                                               double tolerance);

/// The points in one place with a point, that point among them: the ends of the curves that meet
/// at a corner, say.
[[nodiscard]] std::vector<EntityId> pointsTogether(const Sketch& sketch, EntityId point);

/// Where two curves cross or touch, within the extent of each. Empty for anything but lines,
/// circles, and arcs.
[[nodiscard]] std::vector<Point2> intersections(const Sketch& sketch, EntityId first,
                                                EntityId second);

// Shapes made of several curves. Each throws std::invalid_argument for sizes that describe
// nothing, and then leaves the sketch as it was.

struct RectangleParts {
    EntityId bottom = 0;
    EntityId right = 0;
    EntityId top = 0;
    EntityId left = 0;
};
/// Four lines joined at the corners, two held horizontal and two vertical. The lines run
/// counter-clockwise, starting with the bottom one from left to right.
RectangleParts addRectangle(Sketch& sketch, Point2 corner, Point2 opposite);

struct PolygonParts {
    std::vector<EntityId> sides;
    /// The construction circle through the corners.
    EntityId circle = 0;
};
/// A center rectangle: also a construction diagonal, and a point at its middle, which is where
/// the rectangle is centered.
struct CenterRectangleParts {
    RectangleParts sides;
    EntityId diagonal = 0;
    EntityId center = 0;
};
/// A rectangle around `center` with one corner at `corner`, held centered on the point.
CenterRectangleParts addCenterRectangle(Sketch& sketch, Point2 center, Point2 corner);

/// The circle through three points: its center and radius. Nothing if they lie in a line.
[[nodiscard]] std::optional<std::pair<Point2, double>> circleThrough(Point2 a, Point2 b,
                                                                     Point2 c);

/// An arc from `start` to `end` that passes through `through`. Throws std::invalid_argument
/// if the three lie in a line.
EntityId addArcThrough(Sketch& sketch, Point2 start, Point2 through, Point2 end);

/// An arc that goes on from the end of a line or an arc at `from` (that end, or a point joined
/// to it) to `end`, joined to that end and tangent to the curve. Throws std::invalid_argument
/// if `from` is not an end of a line or an arc, or `end` lies straight on from it.
EntityId addTangentArc(Sketch& sketch, EntityId from, Point2 end);

/// A regular polygon with `sides` sides (3 to 64) around `center`, with one corner at `corner`:
/// lines of equal length whose ends are held on a construction circle.
PolygonParts addPolygon(Sketch& sketch, Point2 center, Point2 corner, int sides);

struct SlotParts {
    EntityId firstArc = 0;
    EntityId secondArc = 0;
    EntityId firstLine = 0;
    EntityId secondLine = 0;
};
/// A slot: half circles of `radius` around `first` and `second`, joined by two tangent lines.
SlotParts addSlot(Sketch& sketch, Point2 first, Point2 second, double radius);

// Constraints for a selection

/// A constraint, spelled out for particular entities.
struct ConstraintChoice {
    ConstraintKind kind = ConstraintKind::Coincident;
    EntityId first = 0;
    EntityId second = 0;
    EntityId third = 0;
    /// For a dimension: what it measures now.
    double value = 0.0;
};

/// What a constraint tool means for the entities selected, in the order they were selected, or
/// nothing if it does not apply to them. "Coincident" on a point and a line means the point
/// lies on the line; on a point and a circle, on the circle. Tools that take two entities of
/// different kinds take them in either order.
[[nodiscard]] std::optional<ConstraintChoice>
constraintFor(const Sketch& sketch, ConstraintKind tool, const std::vector<EntityId>& selection);

/// The dimension a selection gets: a line its length, two points their distance, a circle its
/// diameter, an arc its radius, a point and a line the distance between them, two lines the
/// angle between them or, if they are parallel, the distance.
[[nodiscard]] std::optional<ConstraintChoice> dimensionFor(const Sketch& sketch,
                                                           const std::vector<EntityId>& selection);

/// Why a constraint was not added or a value not changed.
enum class Refusal {
    None,
    /// The entities are not ones the constraint takes, or the value is not usable.
    Invalid,
    /// The other constraints already demand it.
    Redundant,
    /// It contradicts the other constraints.
    Conflicting,
};

struct Outcome {
    Refusal refusal = Refusal::None;
    /// The constraint that was added. 0 if it was refused.
    ConstraintId constraint = 0;
    /// The state of the sketch afterwards.
    SolveResult result;

    [[nodiscard]] bool ok() const { return refusal == Refusal::None; }
};

/// Adds a constraint and solves. A constraint that would add nothing, or that contradicts the
/// others, is refused, and the sketch stays exactly as it was.
Outcome tryConstraint(Sketch& sketch, ConstraintKind kind, EntityId first, EntityId second = 0,
                      EntityId third = 0, double value = 0.0);

/// What stands in the way of a constraint that tryConstraint() refuses. If the same thing is
/// already said, that; otherwise every constraint without which it would go in, and of those,
/// when it is a dimension that is refused, the dimensions if there are any: they are what the
/// user set, and the rest is how the sketch holds together.
[[nodiscard]] std::vector<ConstraintId> blockedBy(const Sketch& sketch, ConstraintKind kind,
                                                  EntityId first, EntityId second = 0,
                                                  EntityId third = 0, double value = 0.0);

/// Changes a dimension's value and solves. If the geometry cannot follow, the value is refused
/// and the sketch stays exactly as it was.
Outcome tryDimensionValue(Sketch& sketch, ConstraintId dimension, double value);

// Modifying curves

/// Adds mirror images of curves and free-standing points in a line, and holds each image
/// symmetric to its original. Returns the new curves and points, in the order of `entities`.
/// Throws std::invalid_argument if `axis` is not a line or is among `entities`.
std::vector<EntityId> mirror(Sketch& sketch, const std::vector<EntityId>& entities, EntityId axis);

/// Copies of curves and free-standing points turned about `center`: `count` in all, the
/// originals among them, spread evenly over `angle` radians (a full turn by default, which
/// leaves no copy on top of the originals). The constraints among the originals, such as a
/// rectangle's or its dimensions, are copied too. Returns the copies. Throws
/// std::invalid_argument for a count below 2 or an entity that cannot be copied.
std::vector<EntityId> patternCircular(Sketch& sketch, const std::vector<EntityId>& entities,
                                      Point2 center, int count,
                                      double angle = 2.0 * 3.14159265358979323846);

/// Copies moved by `step` each time: `count` in all, the originals among them. Otherwise as
/// patternCircular().
std::vector<EntityId> patternRectangular(Sketch& sketch, const std::vector<EntityId>& entities,
                                         Point2 step, int count);

/// How selected geometry is moved: turned by `angle` radians about `center`, made `scale`
/// times as large about it, then moved by `move`.
struct Placing {
    Point2 center;
    double angle = 0.0;
    double scale = 1.0;
    Point2 move;
};

/// Moves curves and free-standing points as `placing` says, keeping the constraints among them,
/// and solves; or, with `copy`, adds moved copies and leaves them where they are. Returns what
/// was moved or made. Throws std::invalid_argument, leaving the sketch as it was, if the other
/// constraints do not let them go there, such as a dimension when they are scaled.
std::vector<EntityId> place(Sketch& sketch, const std::vector<EntityId>& entities,
                            const Placing& placing, bool copy);

/// Adds copies of lines, arcs, and circles at `distance` from the originals, on the side
/// `side` is on. Curves that are joined end to end are offset as one outline, and the copy is
/// joined at its corners too. Each new line is held parallel to its original and each new arc or
/// circle concentric with its original; the first line of an outline also gets the distance as a
/// dimension. Returns the new curves. Throws std::invalid_argument if the distance is not
/// positive or leaves nothing of a curve.
std::vector<EntityId> offset(Sketch& sketch, const std::vector<EntityId>& curves, double distance,
                             Point2 side);

/// Removes the part of a curve that `nearPoint` is on, up to where other curves cross it. A curve
/// that nothing crosses is removed whole. The cut ends are held on the curves that cut them.
/// Returns false, changing nothing, if there is nothing to trim.
bool trim(Sketch& sketch, EntityId curve, Point2 nearPoint);

/// Lengthens a line or an arc at the end nearer to `nearPoint` until it meets another curve, and
/// holds that end on the curve. Returns false, changing nothing, if it meets none.
bool extend(Sketch& sketch, EntityId curve, Point2 nearPoint);

/// What fillet() made: the arc that rounds the corner, and the point where the corner was.
struct FilletParts {
    EntityId arc = 0;
    /// A construction point held on both lines where they would meet, which the constraints
    /// and dimensions that were on the corner are now on.
    EntityId corner = 0;
    /// The arc's radius.
    ConstraintId radius = 0;
};

/// Rounds the corner where two lines meet at `corner` (an end of either) with an arc of
/// `radius`: shortens both lines, joins the arc to them and holds it tangent to both, and gives
/// it its radius as a dimension. What was on the corner moves to a construction point where
/// the lines would meet, so a dimension to the corner still holds. Throws std::invalid_argument,
/// leaving the sketch as it was, if two lines do not meet there, they are in line, or the arc
/// does not fit on them.
FilletParts fillet(Sketch& sketch, EntityId corner, double radius);

/// What chamfer() made: the line across the corner, and the point where the corner was.
struct ChamferParts {
    EntityId line = 0;
    /// As FilletParts::corner.
    EntityId corner = 0;
    /// How far back along each line the corner is cut: a dimension on one of two construction
    /// lines from the corner, which are held equal.
    ConstraintId distance = 0;
};

/// Cuts the corner where two lines meet at `corner` with a straight line, `distance` back along
/// each. Otherwise as fillet().
ChamferParts chamfer(Sketch& sketch, EntityId corner, double distance);

} // namespace lcad::sketch
