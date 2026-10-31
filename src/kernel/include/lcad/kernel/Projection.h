// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/Plane.h>
#include <lcad/kernel/Shape.h>

#include <optional>
#include <string>
#include <vector>

// Solids seen from a direction: what a drawing shows of them, and what a machining operation
// follows. Everything here is in millimeters, flat, in the coordinates of the view.
namespace lcad::kernel {

/// A point on a flat view.
struct Point2D {
    double x = 0.0;
    double y = 0.0;

    friend bool operator==(const Point2D&, const Point2D&) = default;
};

/// Where a view looks from: the model's directions that run to the right and up on it. The
/// viewer is on the side `right` crossed with `up` points to. Both are of unit length, and
/// square to each other.
struct ViewFrame {
    Vector right{1.0, 0.0, 0.0};
    Vector up{0.0, 0.0, 1.0};
};

/// A point of the model as the view shows it.
[[nodiscard]] Point2D projected(const ViewFrame& frame, const Point& point);
/// How far a point of the model is toward the viewer.
[[nodiscard]] double depthOf(const ViewFrame& frame, const Point& point);

/// One curve of a view. `points` always follows it, closely enough to draw; a straight line has
/// two, and a circle or part of one says so besides, so that a file can keep it exact.
struct ProjectedCurve {
    std::vector<Point2D> points;
    /// Set for a circle or part of one: counter-clockwise from `startAngle` to `endAngle`, in
    /// radians. A whole circle ends 2 pi after it starts.
    bool isArc = false;
    Point2D center;
    double radius = 0.0;
    double startAngle = 0.0;
    double endAngle = 0.0;
};

/// What a view shows of a shape.
struct Projection {
    /// Edges and outlines that can be seen.
    std::vector<ProjectedCurve> visible;
    /// Edges between faces that run smoothly into each other, which can be seen. A drawing leaves
    /// these out or draws them thin.
    std::vector<ProjectedCurve> smooth;
    /// Edges and outlines that something is in front of.
    std::vector<ProjectedCurve> hidden;
};

/// Removes what cannot be seen of `shape` from where `frame` looks, and flattens the rest.
/// `tolerance` is how far the points of a curve may lie from it. Throws kernel::Error if it cannot
/// be worked out.
[[nodiscard]] Projection project(const Shape& shape, const ViewFrame& frame,
                                 double tolerance = 0.02);

/// A flat region: its outline, and the outlines of the holes in it.
struct FlatRegion {
    std::vector<Point2D> outer;
    std::vector<std::vector<Point2D>> holes;
};

/// A solid cut through.
struct SectionCut {
    /// What is left on the side of the plane its normal points away from.
    Shape behind;
    /// The cut faces, in the coordinates of `frame`.
    std::vector<FlatRegion> regions;
};

/// Cuts `shape` through with a plane, and keeps the part behind it. Regions are in the
/// coordinates of `frame`. Throws kernel::Error if the plane misses the shape.
[[nodiscard]] SectionCut sectionBy(const Shape& shape, const Plane& plane, const ViewFrame& frame,
                                   double tolerance = 0.02);

/// The outlines of a flat face, seen along its normal from outside the solid, in the plane's
/// coordinates: u to the right, v up. Throws kernel::Error if the face is not flat.
[[nodiscard]] FlatRegion flatOutline(const Shape& face, const Plane& plane,
                                     double tolerance = 0.02);

/// A round face whose axis runs along Z: the side of a hole.
struct RoundFace {
    Point2D center;
    double radius = 0.0;
    double bottom = 0.0;
    double top = 0.0;
};

/// A face as a round one along Z, if that is what it is.
[[nodiscard]] std::optional<RoundFace> roundFaceAlongZ(const Shape& face);

/// The plane that lies at height `z`, looked at from above: u along X and v along Y. What
/// flatOutline() gives on it is in the model's own X and Y.
[[nodiscard]] Plane levelPlane(double z);

/// What can be said of an edge, for hanging a dimension on it.
struct EdgeSummary {
    std::string name;
    Point start;
    Point end;
    Point middle;
    /// A straight edge's length in the model.
    bool isLine = false;
    /// A circle or part of one.
    bool isCircle = false;
    Point center;
    double radius = 0.0;
    /// The direction square to a circle's plane.
    Vector axis;
};

/// Every edge of a shape, under the name edgeNames() gives it.
[[nodiscard]] std::vector<EdgeSummary> edgeSummaries(const NamedShape& shape);

} // namespace lcad::kernel
