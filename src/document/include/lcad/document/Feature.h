// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/ObjectId.h>
#include <lcad/sketch/Sketch.h>

#include <optional>
#include <string>
#include <vector>

namespace lcad::document {

/// What a feature does.
enum class FeatureKind {
    /// Pulls outlines of a sketch into a solid. What other programs call extrude.
    Pull,
    /// Turns outlines of a sketch about a line of the sketch.
    Revolve,
    /// Rounds edges.
    Fillet,
    /// Bevels edges.
    Chamfer,
    /// Hollows a body out.
    Shell,
    /// Adds the mirror image of features, or of the whole body, in a plane.
    Mirror,
    /// Repeats features, or the whole body, in rows and columns.
    RectangularPattern,
    /// Repeats features, or the whole body, around an axis.
    CircularPattern,
    /// A construction plane, which sketches can be drawn on: an origin plane or a flat face of a
    /// body, moved along its normal. It makes no solid.
    Plane,
    /// Holes drilled into a body at the points of a sketch.
    Hole,
};

[[nodiscard]] const char* toString(FeatureKind kind);
[[nodiscard]] std::optional<FeatureKind> featureKindFromString(const std::string& name);

/// What a pull or a revolve does with the solid it makes.
enum class Operation {
    NewBody,   ///< it becomes a body of its own
    Join,      ///< it is added to a body
    Cut,       ///< it is taken out of a body
    Intersect, ///< only what it shares with a body is kept
};

[[nodiscard]] const char* toString(Operation operation);
[[nodiscard]] std::optional<Operation> operationFromString(const std::string& name);

/// What a hole looks like at the top.
enum class HoleKind {
    Simple,
    /// Wider for a depth, with a flat step: for the head of a cap screw.
    Counterbore,
    /// Wider in a cone: for the head of a flat screw.
    Countersink,
};

[[nodiscard]] const char* toString(HoleKind kind);
[[nodiscard]] std::optional<HoleKind> holeKindFromString(const std::string& name);

/// One step in the making of the model: what to do, to what, and by how much. The document
/// keeps features in the order they are applied, and builds the bodies from them.
///
/// A feature refers to what it works on in ways that survive the model being built again: a
/// sketch and a body by their ids, an outline by the sketch curves it is made of, an edge or a
/// face by its name (see kernel::NamedShape).
struct Feature {
    ObjectId id;
    std::string name;
    FeatureKind kind = FeatureKind::Pull;
    /// A suppressed feature stays in the timeline and is not applied.
    bool suppressed = false;

    /// The body the feature changes. For Operation::NewBody: the body it makes.
    ObjectId body;

    // Pull and Revolve

    /// The sketch the outlines are taken from.
    ObjectId sketch;
    /// The regions used, each given by the curves of its outer outline, sorted. Empty means
    /// every region of the sketch.
    std::vector<std::vector<sketch::EntityId>> regions;
    Operation operation = Operation::NewBody;

    // Pull

    /// How far, in millimeters, along the sketch plane's normal. Negative goes the other way.
    double distance = 10.0;
    /// For a pull to both sides: how far the other way. Zero for a pull to one side.
    double distanceBack = 0.0;
    /// Half of `distance` to each side of the sketch.
    bool symmetric = false;

    // Revolve

    /// The line of the sketch to turn about.
    sketch::EntityId axis = 0;
    /// In degrees, up to 360.
    double angle = 360.0;

    // Fillet, Chamfer, and Shell

    /// Fillet and Chamfer: the edges, by name.
    std::vector<std::string> edges;
    /// Shell: the faces to leave open, by name.
    std::vector<std::string> faces;
    /// The radius of a fillet, the distance of a chamfer, or the wall thickness of a shell.
    double size = 1.0;
    // Mirror and patterns
    /// The features that are copied: pulls and revolves that make or change `body`, each copy
    /// doing to the body what its original does. Empty means the body itself is copied and
    /// joined to it.
    std::vector<ObjectId> sources;
    /// Mirror: the plane to mirror in: "XY", "XZ", "YZ", or the name of a flat face of the
    /// body. Patterns: the direction to repeat along, or the axis to repeat around: "X", "Y",
    /// "Z", or the name of an edge or a face of the body (see kernel::axisOf). Plane: what it
    /// is placed against, an origin plane or a flat face of `body`; `distance` is how far
    /// along that plane's normal it is.
    std::string reference;
    /// Rectangular pattern: the second direction, like `reference`. Empty for none.
    std::string reference2;
    /// How many there are in all, the original included: along `reference`, around the axis.
    int count = 2;
    /// Rectangular pattern: how many along `reference2`.
    int count2 = 1;
    /// Rectangular pattern: the distance from one to the next, in millimeters, along each
    /// direction. Negative goes the other way. Circular pattern: `angle` is the angle they are
    /// spread over; 360 spreads them evenly all the way round.
    double spacing = 10.0;
    double spacing2 = 10.0;
    /// Plane: whether it is shown.
    bool visible = true;
    // Hole. Its sketch's free-standing points are where the holes go, its `size` is their
    // diameter and its `distance` their depth.
    HoleKind holeKind = HoleKind::Simple;
    /// Through the whole body, whatever its size, with a flat bottom.
    bool throughAll = false;
    /// The angle of the point a drill leaves at the bottom, in degrees; 180 for a flat bottom.
    double tipAngle = 118.0;
    /// Counterbore and countersink: the diameter at the top, and the depth of a counterbore.
    double headDiameter = 10.0;
    double headDepth = 3.0;
    /// Countersink: the angle of its cone, in degrees.
    double countersinkAngle = 90.0;
    /// Drilled along the sketch's normal rather than against it: from the sketch's front, the
    /// side it is looked at from, toward its back is the usual way.
    bool flipped = false;

    friend bool operator==(const Feature&, const Feature&) = default;
};

} // namespace lcad::document
