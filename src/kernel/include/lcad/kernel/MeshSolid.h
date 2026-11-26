// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/kernel/MeshSurfaces.h>
#include <lcad/kernel/Shape.h>

#include <vector>

// Exact solids made from the surfaces of a mesh. See findSurfaces().
namespace lcad::kernel {

/// What solidsFromMesh() made.
struct MeshSolids {
    /// One for each closed part of the mesh, with the part's own holes and hollows.
    std::vector<Shape> solids;
    /// Parts of the mesh that do not close, which no solid can be made of.
    int openParts = 0;
    /// Closed parts whose exact solid came out wrong, and that were made of flat facets instead.
    int facetedParts = 0;
    /// Closed parts of which nothing could be made.
    int failedParts = 0;
};

/// Makes solids of exact faces: a face for each surface, cut off by the curves in which it meets
/// the next. What fits no surface becomes flat facets, as the mesh has it. Throws kernel::Error
/// when stopped (see Stop.h).
[[nodiscard]] MeshSolids solidsFromMesh(const Mesh& mesh, const MeshSurfaces& found);

/// The volume a closed mesh encloses, in cubic millimeters.
[[nodiscard]] double meshVolume(const Mesh& mesh);

} // namespace lcad::kernel
