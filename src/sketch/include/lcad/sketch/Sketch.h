// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// A 2D sketch: geometry on a plane, and the constraints that tie it together. Coordinates are in
// the sketch's own plane, in millimeters; angles are in radians. Where the plane sits in space is
// not this module's business.
namespace lcad::sketch {

/// Identifies an entity within its sketch. Never reused. 0 means none.
using EntityId = std::uint32_t;
/// Identifies a constraint within its sketch. Never reused. 0 means none.
using ConstraintId = std::uint32_t;

struct Point2 {
    double x = 0.0;
    double y = 0.0;
};

enum class EntityKind { Point, Line, Circle, Arc, Ellipse, Spline };

/// A piece of sketch geometry.
///
/// Every position is a Point entity. A line has two of its own, a circle one (its center), and
/// an arc three (center, start, end; the arc runs counter-clockwise from start to end). An
/// ellipse has its center and, as `start`, the end of its major axis, the point where the
/// ellipse is farthest from its center; `radius` is its minor radius. A spline has its control
/// points, in order, of which the first is its `start` and the last its `end`: it runs through
/// those two and is drawn toward the others. Those points belong to their curve: they are
/// created with it and go when it goes. Constraints refer to points and curves alike by id.
struct Entity {
    EntityId id = 0;
    EntityKind kind = EntityKind::Point;
    /// Construction geometry guides the sketch and is left out of profiles.
    bool construction = false;
    /// Reference geometry has a place of its own, and the solver leaves it there. The sketch's
    /// origin point is the first of these.
    bool fixed = false;
    /// For a point that is an end or the center of a curve: that curve. 0 for a point on its own.
    EntityId owner = 0;

    // Point
    double x = 0.0;
    double y = 0.0;
    // Line: start and end. Circle: center. Arc: center, start, and end. Ellipse: center, and
    // the end of its major axis as start. Spline: its first and last control points.
    EntityId center = 0;
    EntityId start = 0;
    EntityId end = 0;
    /// A circle's radius; an ellipse's minor radius. An arc's radius is the distance from its
    /// center to its start.
    double radius = 0.0;
    /// Spline only: all its control points, in order.
    std::vector<EntityId> points;
};

/// The degree of a spline with `controlPoints` control points: cubic, or less if there are too
/// few for that. Its knots are spread evenly, and it is clamped: it starts at its first control
/// point and ends at its last.
[[nodiscard]] int splineDegree(std::size_t controlPoints);

enum class ConstraintKind {
    // Geometric. The comment gives what `first`, `second`, and `third` have to be.
    Coincident,    ///< point, point
    PointOnLine,   ///< point, line: the point lies on the line, extended if need be
    PointOnCircle, ///< point, circle or arc
    Horizontal,    ///< line; or point, point
    Vertical,      ///< line; or point, point
    Parallel,      ///< line, line
    Perpendicular, ///< line, line
    Tangent,       ///< line and circle or arc, in either order; or two of circle and arc
    Equal,         ///< line, line (same length); or two of circle and arc (same radius)
    Concentric,    ///< two of circle and arc
    Midpoint,      ///< point, line
    Symmetric,     ///< point, point, line: mirror images of each other in the line
    Collinear,     ///< line, line
    Fix,           ///< point: stays where it was when the constraint was added

    // Dimensions. Each has a value.
    Distance,          ///< point, point
    DistanceX,         ///< point, point: distance along X
    DistanceY,         ///< point, point: distance along Y
    PointLineDistance, ///< point, line
    Radius,            ///< circle or arc
    Diameter,          ///< circle or arc
    Angle,             ///< line, line: from the first's direction counter-clockwise to the second's
};

/// Whether a kind of constraint carries a value.
[[nodiscard]] bool isDimension(ConstraintKind kind);
/// The name a kind goes by: "Coincident", "Distance".
[[nodiscard]] const char* toString(ConstraintKind kind);
/// The kind with that name, if there is one.
[[nodiscard]] std::optional<ConstraintKind> constraintKindFromString(const std::string& name);

struct Constraint {
    ConstraintId id = 0;
    ConstraintKind kind = ConstraintKind::Coincident;
    EntityId first = 0;
    EntityId second = 0;
    EntityId third = 0;
    /// The dimension's value, in millimeters or radians. For Fix: the point's X.
    double value = 0.0;
    /// For Fix: the point's Y.
    double value2 = 0.0;
};

enum class SolveStatus {
    /// Every constraint is met.
    Solved,
    /// Every constraint is met, and some add nothing the others do not already demand.
    Redundant,
    /// Some constraints contradict the rest. The rest are met; those are named and left out.
    Conflicting,
};

/// What solving found out about a sketch.
struct SolveResult {
    SolveStatus status = SolveStatus::Solved;
    /// How many independent ways the geometry can still move.
    int degreesOfFreedom = 0;
    /// Constraints that add nothing, in the order they were added.
    std::vector<ConstraintId> redundant;
    /// Constraints that cannot be met together with those added before them.
    std::vector<ConstraintId> conflicting;
    /// Curves and free-standing points that can still move. Sorted.
    std::vector<EntityId> underconstrained;

    [[nodiscard]] bool isFullyConstrained() const {
        return status == SolveStatus::Solved && degreesOfFreedom == 0;
    }
};

/// A closed outline made of lines and arcs joined end to end, or a single circle.
struct Profile {
    /// The curves, in order around the outline.
    std::vector<EntityId> curves;
    /// The area the outline encloses, in square millimeters.
    double area = 0.0;
};

/// A part of the plane to make a solid from: the inside of one closed outline, less the insides
/// of the outlines that lie directly within it.
struct Region {
    /// The outline around the region, as Profile::curves.
    std::vector<EntityId> outer;
    /// The outlines of the holes in it.
    std::vector<std::vector<EntityId>> holes;
    /// The area of the region itself: what the outer outline encloses, less its holes.
    double area = 0.0;
};

/// A sketch. It is a value: copying one copies its geometry and constraints, which is how an
/// edit is tried and taken back.
class Sketch {
public:
    /// A new sketch holds one entity: its origin, a fixed point at (0, 0).
    Sketch();

    /// The origin point.
    [[nodiscard]] EntityId origin() const { return m_origin; }

    // Geometry. Each of these throws std::invalid_argument for input that is not finite or
    // describes nothing (a line of no length, a radius that is not positive).

    EntityId addPoint(Point2 position);
    EntityId addLine(Point2 start, Point2 end);
    EntityId addCircle(Point2 center, double radius);
    /// An arc around `center` from `start` counter-clockwise to the direction of `end`. The end
    /// point is placed at the start's distance from the center.
    EntityId addArc(Point2 center, Point2 start, Point2 end);
    /// An ellipse around `center`, with `majorEnd` at the end of its major axis and the given
    /// minor radius, which has to be smaller than the major one.
    EntityId addEllipse(Point2 center, Point2 majorEnd, double minorRadius);
    /// A spline through its first and last control points, drawn toward the others. It needs
    /// three control points or more, no two in the same place.
    EntityId addSpline(const std::vector<Point2>& controlPoints);

    /// Removes a curve or a free-standing point, the points that belong to it, and every
    /// constraint that refers to any of them. Given a point of a curve, removes the curve.
    /// The origin cannot be removed.
    void remove(EntityId id);
    void setConstruction(EntityId id, bool construction);
    /// Makes a curve or a free-standing point reference geometry, which has a place of its own
    /// that the solver leaves alone, or ordinary geometry again. Not for the origin.
    void setFixed(EntityId id, bool fixed);

    [[nodiscard]] const std::vector<Entity>& entities() const { return m_entities; }
    /// The entity, or nullptr.
    [[nodiscard]] const Entity* find(EntityId id) const;
    /// Where a point entity is. Throws std::invalid_argument if `id` is not a point.
    [[nodiscard]] Point2 position(EntityId point) const;
    /// The radius of a circle or an arc; the minor radius of an ellipse.
    [[nodiscard]] double radiusOf(EntityId curve) const;
    /// The point at `t`, from 0 to 1, along a curve: from its start to its end for a line, an
    /// arc, or a spline; once round, counter-clockwise from the end of the major axis or from
    /// the point to the right of the center, for an ellipse or a circle.
    [[nodiscard]] Point2 pointAt(EntityId curve, double t) const;
    /// `pieces` + 1 points along a curve, evenly in `t` (see pointAt()). For drawing it, and for
    /// telling what is near it.
    [[nodiscard]] std::vector<Point2> samples(EntityId curve, int pieces) const;

    // Constraints

    /// Adds a constraint and returns its id. For Fix, the point's current position is recorded.
    /// Throws std::invalid_argument if the entities are not of the kinds the constraint takes,
    /// or a dimension's value is not usable. Does not solve.
    ConstraintId addConstraint(ConstraintKind kind, EntityId first, EntityId second = 0,
                               EntityId third = 0, double value = 0.0);
    void removeConstraint(ConstraintId id);
    /// Changes a dimension's value. Does not solve.
    void setConstraintValue(ConstraintId id, double value);
    [[nodiscard]] const std::vector<Constraint>& constraints() const { return m_constraints; }
    [[nodiscard]] const Constraint* findConstraint(ConstraintId id) const;
    /// What a dimension would read if it were measured now, whatever its stored value.
    [[nodiscard]] double measure(ConstraintKind kind, EntityId first, EntityId second = 0) const;

    // Solving

    /// Moves the geometry, as little as possible, until the constraints are met, and reports.
    SolveResult solve();
    /// What solve() would report, without moving anything.
    [[nodiscard]] SolveResult state() const;
    /// Moves a point, or a curve as a whole, by the given amount as far as the constraints
    /// allow, and solves. What cannot move stays.
    SolveResult dragBy(EntityId id, double dx, double dy);
    /// Makes a circle's radius the distance from its center to `toward` as far as the
    /// constraints allow, and solves.
    SolveResult dragRadius(EntityId circle, Point2 toward);

    // Analysis

    /// The closed outlines among the sketch's ordinary (not construction) curves.
    [[nodiscard]] std::vector<Profile> profiles() const;
    /// The regions the closed outlines divide the plane into, one per outline, in the order of
    /// profiles(). A circle inside a rectangle gives two: the rectangle with a round hole, and
    /// the disc.
    [[nodiscard]] std::vector<Region> regions() const;
    /// The points of a closed outline, in order around it, with arcs and circles cut into
    /// short chords. For telling what lies inside it, and for drawing it filled.
    [[nodiscard]] std::vector<Point2> outlinePolygon(const std::vector<EntityId>& curves) const;

    // For reading a sketch back from a file.

    [[nodiscard]] EntityId nextEntityId() const { return m_nextEntity; }
    [[nodiscard]] ConstraintId nextConstraintId() const { return m_nextConstraint; }
    /// Rebuilds a sketch from stored entities and constraints. Throws std::invalid_argument
    /// when they do not fit together.
    Sketch(std::vector<Entity> entities, std::vector<Constraint> constraints, EntityId nextEntity,
           ConstraintId nextConstraint);

private:
    friend struct SolverAccess;

    Entity& entity(EntityId id);
    [[nodiscard]] const Entity& entity(EntityId id) const;
    EntityId addOwnedPoint(Point2 position, EntityId owner);
    void validate(const Constraint& constraint) const;

    std::vector<Entity> m_entities;
    std::vector<Constraint> m_constraints;
    EntityId m_origin = 0;
    EntityId m_nextEntity = 1;
    ConstraintId m_nextConstraint = 1;
};

} // namespace lcad::sketch
