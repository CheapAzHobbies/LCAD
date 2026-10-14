// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/Document.h>

#include <stdexcept>
#include <string>
#include <unordered_map>

// Applying one feature. Private to the document module.
namespace lcad::document::detail {

/// The bodies as they are when a feature's turn comes.
using BodyStates = std::unordered_map<ObjectId, kernel::NamedShape>;

/// A feature that cannot be applied. The message is a sentence for the user.
class FeatureFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// What a feature makes of its body, given the bodies as they are when its turn comes. Throws
/// FeatureFailure if it cannot be applied. For a pull or a revolve, `tool`, if given, receives
/// the solid it makes before that is combined with a body, also when combining fails.
[[nodiscard]] kernel::NamedShape evaluate(const Feature& feature, const Document& document,
                                          const BodyStates& bodies,
                                          kernel::NamedShape* tool = nullptr);

/// Where a construction plane goes, given the bodies as they are when its turn comes. Throws
/// FeatureFailure if it cannot be placed.
[[nodiscard]] PlaneState placePlane(const Feature& feature, const Document& document,
                                    const BodyStates& bodies);

/// Where a construction axis goes, given the bodies as they are when its turn comes. Throws
/// FeatureFailure if it cannot be placed.
[[nodiscard]] AxisState placeAxis(const Feature& feature, const Document& document,
                                  const BodyStates& bodies);

/// Where an attached sketch goes, given the bodies and the construction planes as they are
/// when its turn comes. Throws FeatureFailure if what it is attached to is not there.
[[nodiscard]] kernel::Plane placeSketch(const SketchItem& sketch, const Document& document,
                                        const BodyStates& bodies,
                                        const std::unordered_map<ObjectId, PlaneState>& planes);

/// What a face's or an edge's name refers to, in words: "a side of Pull 1".
[[nodiscard]] std::string describeName(const Document& document, const std::string& name);

} // namespace lcad::document::detail
