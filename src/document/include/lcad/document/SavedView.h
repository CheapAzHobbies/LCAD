#pragma once

#include <lcad/document/ObjectId.h>

#include <array>
#include <string>

namespace lcad::document {

/// A camera position kept with the document under a name, to go back to.
struct SavedView {
    ObjectId id;
    std::string name;
    /// Where the camera is, the point it looks at, and which way is up, in millimeters.
    std::array<double, 3> eye{};
    std::array<double, 3> center{};
    std::array<double, 3> up{0.0, 0.0, 1.0};
    /// How much of the model the view holds: the camera's scale.
    double scale = 1.0;
    bool perspective = false;

    friend bool operator==(const SavedView&, const SavedView&) = default;
};

} // namespace lcad::document
