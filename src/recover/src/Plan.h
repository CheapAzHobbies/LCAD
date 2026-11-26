// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/kernel/Exchange.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/MeshSurfaces.h>
#include <lcad/kernel/Plane.h>
#include <lcad/kernel/Projection.h>

#include <optional>
#include <string>
#include <vector>

// How a part made of layers could have been modeled, worked out from its surfaces.
//
// Most machined and printed parts are stacks of outlines along one direction, with their edges
// rounded and holes drilled. Along that direction every face is a side, parallel to it, or a level,
// square to it. Between two levels a cut through the part is the same all the way, so one cut gives
// the sketch for that layer; rounds and bevels are left out of the outline, and put back afterward
// as the fillets and chamfers they are.
namespace lcad::recover {

/// A straight line or a part of a circle in a sketch's plane.
struct Curve {
    bool round = false;
    /// A line from `start` to `end`, or an arc around `center` from `start` to `end`, going
    /// counter-clockwise if `counterClockwise`.
    kernel::Point2D start;
    kernel::Point2D end;
    kernel::Point2D center;
    double radius = 0.0;
    bool counterClockwise = true;
    /// A whole circle: `start` and `end` mean nothing.
    bool whole = false;
};

/// A closed outline, inner ones running the other way round.
struct Outline {
    std::vector<Curve> curves;
    double area = 0.0; ///< with a sign: positive for an outer one
    /// Says that two outlines are the same.
    std::string signature;
};

/// A stretch along the direction with one set of outlines.
struct Layer {
    double low = 0.0;
    double high = 0.0;
    std::vector<Outline> outlines;
};

struct PlannedHole {
    kernel::Plane plane; ///< with the normal pointing out of the part, at the opening
    kernel::Point2D at;
    double diameter = 0.0;
    double depth = 0.0;
    /// The angle of the point at the bottom, in degrees. 180 for a flat bottom.
    double tipAngle = 180.0;
    /// All the way through what is there.
    bool through = false;
};

/// An edge to round or bevel: a place on it, and by how much.
struct PlannedEdge {
    kernel::Point at;
    double size = 0.0;
    bool chamfer = false;
};

struct Plan {
    /// The plane through the origin that sketches lie parallel to. Heights go along its normal.
    kernel::Plane base;
    std::vector<Layer> layers;
    std::vector<PlannedHole> holes;
    /// In the order to round them. Those of one size next to each other are one feature.
    std::vector<PlannedEdge> edges;
};

/// A plan for a closed part, or nothing if it is not one that can be planned, with why in `why`.
[[nodiscard]] std::optional<Plan> planPart(const kernel::Mesh& mesh,
                                           const kernel::MeshSurfaces& found, std::string& why);

} // namespace lcad::recover
