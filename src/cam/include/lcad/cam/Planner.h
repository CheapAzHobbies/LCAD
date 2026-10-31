// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/cam/Toolpath.h>
#include <lcad/document/Document.h>
#include <lcad/kernel/Projection.h>

#include <string>
#include <vector>

// Working out toolpaths. Each kind of operation has a function that takes plain geometry, and
// planSetup() finds the geometry for them in the document. What a function cannot do safely it
// does not do: it says why, in `problems`, and gives no moves.
namespace lcad::cam {

/// The heights an operation moves between, in the model's Z.
struct Heights {
    /// Rapid moves travel here.
    double clearance = 10.0;
    /// A cut starts from here, at the plunge feed.
    double retract = 5.0;
    /// Where the material starts.
    double top = 0.0;
    /// How far down it cuts.
    double bottom = 0.0;
};

/// The depths of the passes from `top` down to `bottom`: each `stepdown` below the one before,
/// and the last exactly at `bottom`.
[[nodiscard]] std::vector<double> passDepths(double top, double bottom, double stepdown);

/// Levels the stock's top: lines to and fro across the whole of it, from outside.
[[nodiscard]] std::vector<Move> faceMoves(const kernel::BoundingBox& stock, const Heights& heights,
                                          const document::Tool& tool,
                                          const document::MachiningOperation& operation,
                                          std::vector<std::string>& problems);

/// Clears a flat area, around its islands, down to `heights.bottom`: rings from the middle
/// outward, each stepover from the next, and the last the tool's radius from the walls.
[[nodiscard]] std::vector<Move> pocketMoves(const kernel::FlatRegion& region,
                                            const Heights& heights, const document::Tool& tool,
                                            const document::MachiningOperation& operation,
                                            std::vector<std::string>& problems);

/// Cuts around an outline, on the side the operation says, in passes down to `heights.bottom`.
[[nodiscard]] std::vector<Move> contourMoves(const kernel::FlatRegion& region,
                                             const Heights& heights, const document::Tool& tool,
                                             const document::MachiningOperation& operation,
                                             std::vector<std::string>& problems);

/// Drills holes, nearest next, each to its own depth. `heights.bottom` is not used.
[[nodiscard]] std::vector<Move> drillMoves(const std::vector<kernel::RoundFace>& holes,
                                           double partBottom, const Heights& heights,
                                           const document::Tool& tool,
                                           const document::MachiningOperation& operation,
                                           std::vector<std::string>& problems);

/// The stock and the machine's zero for a setup: the box of the bodies shown, with the setup's
/// margins. A document with no body shown has an empty one.
[[nodiscard]] WorkArea workAreaOf(const document::Document& document, const document::Setup& setup);

/// Works out every operation of a setup that is not suppressed, in order, and checks what
/// comes of it against the tools and the machine.
[[nodiscard]] Program planSetup(const document::Document& document, const document::Setup& setup);

} // namespace lcad::cam
