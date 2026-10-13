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

/// Changes a dimension's value and solves. If the geometry cannot follow, the value is refused
/// and the sketch stays exactly as it was.
Outcome tryDimensionValue(Sketch& sketch, ConstraintId dimension, double value);

// Modifying curves

/// Adds mirror images of curves and free-standing points in a line, and holds each image
/// symmetric to its original. Returns the new curves and points, in the order of `entities`.
/// Throws std::invalid_argument if `axis` is not a line or is among `entities`.
std::vector<EntityId> mirror(Sketch& sketch, const std::vector<EntityId>& entities, EntityId axis);

/// Adds copies of lines, arcs, and circles at `distance` from the originals, on the side
/// `side` is on. Curves that are joined end to end are offset as one outline, and the copy is
/// joined at its corners too. Each new line is held parallel to its original and each new arc or
/// circle concentric with its original; the first line of an outline also gets the distance as a
/// dimension. Returns the new curves. Throws std::invalid_argument if the distance is not
/// positive or leaves nothing of a curve.
std::vector<EntityId> offset(Sketch& sketch, const std::vector<EntityId>& curves, double distance,
                             Point2 side);

/// Removes the part of a curve that `near` is on, up to where other curves cross it. A curve
/// that nothing crosses is removed whole. The cut ends are held on the curves that cut them.
/// Returns false, changing nothing, if there is nothing to trim.
bool trim(Sketch& sketch, EntityId curve, Point2 near);

/// Lengthens a line or an arc at the end nearer to `near` until it meets another curve, and
/// holds that end on the curve. Returns false, changing nothing, if it meets none.
bool extend(Sketch& sketch, EntityId curve, Point2 near);

} // namespace lcad::sketch
