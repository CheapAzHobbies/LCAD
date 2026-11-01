// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Plan.h"

#include <lcad/kernel/Exchange.h>
#include <lcad/kernel/MeshSurfaces.h>

#include <vector>

// The plain shape under the finishing touches of a mesh.
//
// A round goes by moving the corners of its triangles to where the faces on either side of it would
// meet: the round folds up to a sharp edge, and those faces stretch to it, staying flat. A bevel goes
// the same way. A hole goes by taking its wall out and closing the flat rims it leaves. What comes out
// is a mesh of the same part as it was before these were done to it, which is far easier to read.
namespace lcad::recover {

/// A mesh without its rounds, bevels, and holes, and how to put them back.
struct Sharpened {
    kernel::Mesh mesh;
    /// Its surfaces: those it was found to have, without what went.
    kernel::MeshSurfaces found;
    /// In the order to drill them, before the edges.
    std::vector<PlannedHole> holes;
    /// In the order to round them.
    std::vector<PlannedEdge> edges;
};

/// `keep` says where edges are that are to stay as they are: PlannedEdge::at of those that could not
/// be put back. Throws kernel::Error when stopped (see Stop.h).
[[nodiscard]] Sharpened sharpen(const kernel::Mesh& mesh, const kernel::MeshSurfaces& found,
                                const std::vector<kernel::Point>& keep = {});

} // namespace lcad::recover
