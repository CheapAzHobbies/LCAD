// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Shape.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Geometry going to and from other programs: the exact solids of STEP and IGES files, and the
// triangle meshes of STL, OBJ, and 3MF files that 3D printers and other tools take.
//
// A file that cannot be read or written is a kernel::Error whose reason can be shown to the
// user as it is. Lengths are millimeters on LCAD's side; a file in other units is converted on
// the way in, and what LCAD writes is in millimeters.
namespace lcad::kernel {

/// A shape with the name it has, or is to have, in a file.
struct NamedBody {
    std::string name;
    Shape shape;
};

/// What was read from a STEP or IGES file.
struct ImportResult {
    /// One per solid, and one per set of faces that could not be closed into a solid. Named
    /// after the part in the file where it says, or else after the file.
    std::vector<NamedBody> bodies;
    /// What was done to the geometry on the way in, or is wrong with it: sentences for the user.
    std::vector<std::string> notes;
};

/// Reads the solids of a STEP file (.step, .stp; AP203, AP214, or AP242). Faces that are not
/// part of a solid are sewn together where their edges meet, and a set of them that closes up
/// becomes a solid.
[[nodiscard]] ImportResult importStep(const std::filesystem::path& path);

/// Reads the solids and surfaces of an IGES file (.igs, .iges). The surfaces are sewn together
/// as by importStep().
[[nodiscard]] ImportResult importIges(const std::filesystem::path& path);

/// Writes shapes to a STEP file (AP214), each as a part with its name, in millimeters.
void exportStep(const std::vector<NamedBody>& bodies, const std::filesystem::path& path);

/// How finely a shape is cut into triangles for a mesh file.
struct MeshSettings {
    /// How far the triangles may lie from the surface, in millimeters.
    double deviation = 0.05;
    /// The largest angle between the triangles along a curved surface, in degrees.
    double angle = 15.0;
};

/// The triangles of a shape's surface.
struct Mesh {
    std::vector<Point> vertices;
    /// Each triangle by the indices of its corners, counter-clockwise when seen from outside.
    /// A corner where faces meet is one vertex, so the mesh of a solid is closed.
    std::vector<std::array<std::uint32_t, 3>> triangles;
};

/// Cuts the faces of a shape into triangles. Throws kernel::Error if the shape has no faces.
[[nodiscard]] Mesh triangulate(const Shape& shape, const MeshSettings& settings);

/// Reads the triangles of an STL (binary or text), OBJ, or 3MF file. Corners in the same place
/// become one vertex, and triangles that have collapsed are left out. A 3MF file says what its
/// unit is and comes back in millimeters; the numbers of the other two are taken as they stand,
/// since those formats do not say. Throws kernel::Error for a file without triangles.
[[nodiscard]] Mesh readMesh(const std::filesystem::path& path);

/// How an STL file is written.
enum class StlEncoding {
    Binary, ///< compact; what most programs expect
    Text,   ///< readable, and several times larger
};

/// Writes the meshes of the shapes to one STL file. STL has no names, units, or separate
/// parts: the triangles of all of them follow one another.
void exportStl(const std::vector<NamedBody>& bodies, const std::filesystem::path& path,
               const MeshSettings& settings, StlEncoding encoding = StlEncoding::Binary);

/// Writes the meshes of the shapes to a Wavefront OBJ file, each as an object with its name.
void exportObj(const std::vector<NamedBody>& bodies, const std::filesystem::path& path,
               const MeshSettings& settings);

/// Writes the meshes of the shapes to a 3MF file, each as an object with its name, in
/// millimeters.
void export3mf(const std::vector<NamedBody>& bodies, const std::filesystem::path& path,
               const MeshSettings& settings);

} // namespace lcad::kernel
