// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/kernel/Exchange.h>
#include <lcad/kernel/Measure.h>

#include <cstdint>
#include <vector>

// The surfaces a triangle mesh was cut from. A mesh that a CAD program exported has its corners
// exactly on the surfaces of the model, so the planes, cylinders, cones, spheres, and tori can
// be found again to the precision of the file, not merely guessed at.
namespace lcad::kernel {

enum class SurfaceKind { Plane, Cylinder, Cone, Sphere, Torus };

/// A surface of a mesh, and the triangles that lie on it.
struct MeshSurface {
    SurfaceKind kind = SurfaceKind::Plane;
    /// Plane: a point on it. Cylinder: a point on its axis. Cone: its apex. Sphere and torus:
    /// the center.
    Point origin;
    /// Plane: its normal, out of the material. Cone: from the apex along the surface. Otherwise
    /// the axis, for a sphere of no meaning.
    Vector axis{0.0, 0.0, 1.0};
    /// Cylinder and sphere: the radius. Torus: from the axis to the middle of the tube.
    double radius = 0.0;
    /// Torus: the radius of the tube.
    double tubeRadius = 0.0;
    /// Cone: between the axis and the surface, in radians.
    double halfAngle = 0.0;
    /// Whether the mesh faces away from the axis or center: true for a shaft, false for a hole.
    bool outward = true;
    /// Nothing but a flat piece of something no simple surface fits, such as a free form.
    bool facet = false;
    std::vector<std::uint32_t> triangles;
    double area = 0.0;
};

/// What findSurfaces() found.
struct MeshSurfaces {
    /// Every triangle of the mesh is in exactly one of them.
    std::vector<MeshSurface> surfaces;
    /// For each triangle, the number of its surface.
    std::vector<std::uint32_t> surfaceOf;
    /// How far a corner may be from its surface, in millimeters.
    double tolerance = 0.0;
    /// How much of the mesh's area lies on real surfaces rather than facets, from 0 to 1.
    double share = 0.0;
};

/// The largest distance from its surface a corner of an exactly exported mesh can have, given
/// the mesh's size and that STL files hold single precision numbers.
[[nodiscard]] double defaultMeshTolerance(const Mesh& mesh);

/// Finds the surfaces of a mesh, each corner of a surface's triangles within `tolerance`
/// millimeters of it. A tolerance of zero means defaultMeshTolerance(). Throws kernel::Error
/// when stopped (see Stop.h).
[[nodiscard]] MeshSurfaces findSurfaces(const Mesh& mesh, double tolerance = 0.0);

} // namespace lcad::kernel
