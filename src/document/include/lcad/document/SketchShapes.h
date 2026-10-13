// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/Document.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Shape.h>

#include <optional>

namespace lcad::document {

/// Which of a sketch's geometry to turn into shapes.
enum class SketchPart {
    /// The curves and free-standing points that make up the sketch proper.
    Ordinary,
    /// Construction geometry.
    Construction,
};

/// A sketch's geometry as one shape in model space: an edge for each line, circle, and arc, and
/// a vertex for each free-standing point. A null shape when the sketch has none of that part.
[[nodiscard]] kernel::Shape sketchShape(const SketchItem& sketch, SketchPart part);

/// The box around all of a sketch's geometry in model space, or nothing for an empty sketch.
[[nodiscard]] std::optional<kernel::BoundingBox> sketchBounds(const SketchItem& sketch);

} // namespace lcad::document
