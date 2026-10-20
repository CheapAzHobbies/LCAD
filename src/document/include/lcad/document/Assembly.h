// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/ObjectId.h>
#include <lcad/kernel/Transform.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

// What an assembly is made of: components, their occurrences, the joints between them, and the
// materials bodies are made of. The Document holds them; see Document.h.
namespace lcad::document {

/// A part or a subassembly: bodies, sketches, and features of its own, and occurrences of other
/// components. A component is defined once and appears in the assembly as often as it has
/// occurrences, each with a placement of its own: what is changed in the component changes in
/// every one of them.
///
/// The document itself is the root component. What belongs to no component (its `component` is
/// not a valid id) belongs to the root.
struct Component {
    ObjectId id;
    std::string name;
    /// For a component that is another LCAD document: where that file is, as the user gave it
    /// or relative to this document's folder. Empty for a component made in this document. A
    /// linked component's bodies are copies of that document's, kept here so that this document
    /// opens and shows them when the file cannot be found; it has no sketches or features of its
    /// own, since the file is where it is changed.
    std::string link;
    /// What identifies the file's content as it was when it was last read, so that a change to it
    /// is noticed. Empty for a component that is not linked.
    std::string linkStamp;

    friend bool operator==(const Component&, const Component&) = default;
};

/// One appearance of a component inside another (or inside the document itself): which
/// component, and where it sits.
struct Occurrence {
    ObjectId id;
    /// The component that appears.
    ObjectId component;
    /// The component it appears in. Not valid for the document's root.
    ObjectId parent;
    /// Where it sits in the parent's coordinates, unless joints place it: then this is where it
    /// was last put by hand, and where it goes back to if the joints are deleted.
    kernel::Placement placement;
    /// A grounded occurrence does not move: joints move the other side instead.
    bool grounded = false;
    bool visible = true;
    /// A suppressed occurrence stays in the browser and is otherwise left out: not shown, not
    /// joined, not weighed, not exported.
    bool suppressed = false;

    friend bool operator==(const Occurrence& a, const Occurrence& b) {
        const auto same = [](const kernel::Placement& p, const kernel::Placement& q) {
            return kernel::samePlacement(p, q, 0.0);
        };
        return a.id == b.id && a.component == b.component && a.parent == b.parent &&
               same(a.placement, b.placement) && a.grounded == b.grounded &&
               a.visible == b.visible && a.suppressed == b.suppressed;
    }
};

/// How a joint lets one component move against another.
enum class JointKind {
    /// Not at all.
    Rigid,
    /// It turns about the joint's axis.
    Revolute,
    /// It slides along the joint's axis.
    Slider,
    /// It turns about the joint's axis and slides along it.
    Cylindrical,
    /// It turns about the joint's axis and slides across it, along the joint's X: a pin in a
    /// slot.
    PinSlot,
    /// It slides anywhere in the joint's plane and turns about the axis square to it.
    Planar,
    /// It turns any way about the joint's origin.
    Ball,
};

[[nodiscard]] const char* toString(JointKind kind);
[[nodiscard]] std::optional<JointKind> jointKindFromString(const std::string& name);

/// One way a joint lets its component move: a turn, in degrees, or a slide, in millimeters.
struct JointMotion {
    /// What the value is called: "Rotation", "Slide", "X", "Pitch".
    const char* name;
    bool turns;
};

/// The ways a joint of this kind moves, in the order of Joint::values. None for Rigid.
[[nodiscard]] std::vector<JointMotion> motionsOf(JointKind kind);

/// How far a joint may move one way: the least and the most, where there are such limits.
struct JointLimits {
    std::optional<double> minimum;
    std::optional<double> maximum;

    friend bool operator==(const JointLimits&, const JointLimits&) = default;
};

/// Where on what a joint is placed, on one of its two sides.
struct JointSide {
    /// The occurrences from the joint's component down to the part, outermost first. The first
    /// is the one the joint moves. Empty for the joint's own component, which does not move.
    std::vector<ObjectId> path;
    /// What the frame is taken from: the origin of the component the path leads to, or a face
    /// or an edge of one of its bodies (see kernel::frameOn).
    enum class Kind { Origin, Face, Edge };
    Kind kind = Kind::Origin;
    /// For a face or an edge: the body, and the face's or edge's name.
    ObjectId body;
    std::string name;
    /// For a straight edge or a round face: where along it.
    kernel::FrameSnap snap = kernel::FrameSnap::Center;

    friend bool operator==(const JointSide&, const JointSide&) = default;
};

/// A joint: two frames, one on each of two components, held together. The occurrence on the
/// `moving` side is placed so that its frame lies on the `fixed` side's frame, face to face (Z
/// against Z) unless `flipped`, after `offset` and `angle` and however far the joint has moved.
/// If the moving side cannot move (it is grounded, or another joint already holds it), the
/// fixed side moves instead.
struct Joint {
    ObjectId id;
    std::string name;
    JointKind kind = JointKind::Rigid;
    /// The component the joint is part of, which both sides lie in. Not valid for the root.
    ObjectId component;
    JointSide moving;
    JointSide fixed;
    /// How far the moving side stands off along the joint's Z, in millimeters.
    double offset = 0.0;
    /// How far it is turned about the joint's Z, in degrees.
    double angle = 0.0;
    /// Z of the two frames point the same way, rather than at each other.
    bool flipped = false;
    /// Where the joint is now along each way it moves (see motionsOf()), in degrees or
    /// millimeters.
    std::array<double, 3> values{};
    std::array<JointLimits, 3> limits{};
    /// A suppressed joint holds nothing.
    bool suppressed = false;

    friend bool operator==(const Joint&, const Joint&) = default;
};

/// Where each occurrence is, put together from the root down: one entry for the root and one for
/// every occurrence inside it, and inside those, in turn.
struct Instance {
    /// The occurrences from the root down to this one. Empty for the root itself.
    std::vector<ObjectId> path;
    /// The component that appears here. Not valid for the root.
    ObjectId component;
    /// Where it is in the root's coordinates.
    kernel::Placement placement;
    /// Whether it and every occurrence it is inside are shown.
    bool visible = true;
};

/// What a body is made of, for its mass.
struct Material {
    std::string name;
    /// Grams per cubic centimeter.
    double density = 1.0;

    friend bool operator==(const Material&, const Material&) = default;
};

/// The materials LCAD knows without being told: plastics for printing, metals, wood, in the
/// order they are offered.
[[nodiscard]] const std::vector<Material>& materialLibrary();

/// A body of another document, as a linked component holds it.
struct LinkedBody {
    /// Tells this body apart from the others of the same document, and stays the same when that
    /// document changes, so that what refers to the body still finds it.
    std::string key;
    std::string name;
    /// Placed as it is in the other document.
    kernel::Shape shape;
    std::vector<std::string> faceNames;
    std::string material;
};

/// What a linked component is: the bodies of the other document, and what identifies the
/// version of the file they were read from.
struct LinkedContent {
    std::vector<LinkedBody> bodies;
    std::string stamp;
};

} // namespace lcad::document
