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

    friend bool operator==(const Feature&, const Feature&) = default;
};

} // namespace lcad::document
