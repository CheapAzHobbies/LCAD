// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/document/Feature.h>

#include <algorithm>

namespace lcad::document {

namespace {

struct KindName {
    FeatureKind kind;
    const char* name;
};
constexpr KindName kindNames[] = {
    {FeatureKind::Pull, "Pull"},
    {FeatureKind::Revolve, "Revolve"},
    {FeatureKind::Fillet, "Fillet"},
    {FeatureKind::Chamfer, "Chamfer"},
    {FeatureKind::Shell, "Shell"},
    {FeatureKind::Mirror, "Mirror"},
    {FeatureKind::RectangularPattern, "Rectangular Pattern"},
    {FeatureKind::CircularPattern, "Circular Pattern"},
    {FeatureKind::Plane, "Plane"},
    {FeatureKind::Hole, "Hole"},
    {FeatureKind::Axis, "Axis"},
    {FeatureKind::OffsetFace, "Offset Face"},
    {FeatureKind::MoveFace, "Move Face"},
    {FeatureKind::DeleteFace, "Delete Face"},
    {FeatureKind::Repair, "Repair"},
    {FeatureKind::Loft, "Loft"},
    {FeatureKind::Sweep, "Sweep"},
    {FeatureKind::Draft, "Draft"},
    {FeatureKind::SplitBody, "Split Body"},
    {FeatureKind::Combine, "Combine"},
    {FeatureKind::MoveBody, "Move Body"},
    {FeatureKind::Pipe, "Pipe"},
    {FeatureKind::Coil, "Coil"},
    {FeatureKind::Thread, "Thread"},
    {FeatureKind::SurfacePull, "Surface Pull"},
    {FeatureKind::Patch, "Patch"},
    {FeatureKind::Stitch, "Stitch"},
    {FeatureKind::Thicken, "Thicken"},
    {FeatureKind::OffsetSurface, "Offset Surface"},
    {FeatureKind::SurfaceRevolve, "Surface Revolve"},
};

struct HoleKindName {
    HoleKind kind;
    const char* name;
};
constexpr HoleKindName holeKindNames[] = {
    {HoleKind::Simple, "Simple"},
    {HoleKind::Counterbore, "Counterbore"},
    {HoleKind::Countersink, "Countersink"},
};

struct OperationName {
    Operation operation;
    const char* name;
};
constexpr OperationName operationNames[] = {
    {Operation::NewBody, "NewBody"},
    {Operation::Join, "Join"},
    {Operation::Cut, "Cut"},
    {Operation::Intersect, "Intersect"},
};

} // namespace

const char* toString(FeatureKind kind) {
    for (const KindName& entry : kindNames) {
        if (entry.kind == kind) {
            return entry.name;
        }
    }
    return "Unknown";
}

std::optional<FeatureKind> featureKindFromString(const std::string& name) {
    for (const KindName& entry : kindNames) {
        if (name == entry.name) {
            return entry.kind;
        }
    }
    return std::nullopt;
}

const char* toString(Operation operation) {
    for (const OperationName& entry : operationNames) {
        if (entry.operation == operation) {
            return entry.name;
        }
    }
    return "Unknown";
}

const char* toString(HoleKind kind) {
    for (const HoleKindName& entry : holeKindNames) {
        if (entry.kind == kind) {
            return entry.name;
        }
    }
    return "Unknown";
}

std::optional<HoleKind> holeKindFromString(const std::string& name) {
    for (const HoleKindName& entry : holeKindNames) {
        if (name == entry.name) {
            return entry.kind;
        }
    }
    return std::nullopt;
}

std::optional<Operation> operationFromString(const std::string& name) {
    for (const OperationName& entry : operationNames) {
        if (name == entry.name) {
            return entry.operation;
        }
    }
    return std::nullopt;
}

const char* toString(PlaneKind kind) {
    switch (kind) {
    case PlaneKind::Offset:
        return "Offset";
    case PlaneKind::AtAngle:
        return "At Angle";
    case PlaneKind::Midplane:
        return "Midplane";
    }
    return "Offset";
}

std::optional<PlaneKind> planeKindFromString(const std::string& name) {
    for (const PlaneKind kind : {PlaneKind::Offset, PlaneKind::AtAngle, PlaneKind::Midplane}) {
        if (name == toString(kind)) {
            return kind;
        }
    }
    return std::nullopt;
}

std::vector<ObjectId> sketchesOf(const Feature& feature) {
    std::vector<ObjectId> sketches;
    const auto add = [&sketches](ObjectId id) {
        if (id.isValid() && std::find(sketches.begin(), sketches.end(), id) == sketches.end()) {
            sketches.push_back(id);
        }
    };
    add(feature.sketch);
    for (const LoftSection& section : feature.sections) {
        add(section.sketch);
    }
    add(feature.pathSketch);
    return sketches;
}

} // namespace lcad::document
