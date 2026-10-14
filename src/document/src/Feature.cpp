// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/document/Feature.h>

namespace lcad::document {

namespace {

struct KindName {
    FeatureKind kind;
    const char* name;
};
constexpr KindName kindNames[] = {
    {FeatureKind::Pull, "Pull"},     {FeatureKind::Revolve, "Revolve"},
    {FeatureKind::Fillet, "Fillet"}, {FeatureKind::Chamfer, "Chamfer"},
    {FeatureKind::Shell, "Shell"},
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

std::optional<Operation> operationFromString(const std::string& name) {
    for (const OperationName& entry : operationNames) {
        if (name == entry.name) {
            return entry.operation;
        }
    }
    return std::nullopt;
}

} // namespace lcad::document
