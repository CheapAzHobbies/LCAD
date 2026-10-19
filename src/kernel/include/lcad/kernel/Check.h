// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/kernel/Shape.h>

#include <string>
#include <vector>

namespace lcad::kernel {

/// Something wrong with a shape's geometry, and where.
struct GeometryProblem {
    /// What is wrong, as a sentence for the user: "2 faces cross each other."
    std::string description;
    /// The numbers, as subShape() gives them, of the faces and edges it is about.
    std::vector<int> faces;
    std::vector<int> edges;
};

/// What is wrong with a shape as a solid: invalid faces, edges, and vertices; edges that border
/// only one face, so that it is not closed; faces that face inward; faces that cross each
/// other; edges too short to work with. Nothing for a sound solid.
///
/// Throws std::invalid_argument for a null shape.
[[nodiscard]] std::vector<GeometryProblem> checkGeometry(const Shape& shape);

} // namespace lcad::kernel
