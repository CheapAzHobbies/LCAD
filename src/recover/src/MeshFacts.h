// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/kernel/Exchange.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/MeshSurfaces.h>

#include <Eigen/Dense>

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

// What the surfaces of a mesh are to each other.
namespace lcad::recover {

using Index = std::uint32_t;

[[nodiscard]] inline Eigen::Vector3d vec(const kernel::Point& p) {
    return {p.x, p.y, p.z};
}

[[nodiscard]] inline Eigen::Vector3d vec(const kernel::Vector& v) {
    return {v.x, v.y, v.z};
}

[[nodiscard]] inline kernel::Point point(const Eigen::Vector3d& v) {
    return {v.x(), v.y(), v.z()};
}

[[nodiscard]] inline kernel::Vector direction(const Eigen::Vector3d& v) {
    return {v.x(), v.y(), v.z()};
}

/// How far a point is from a surface, with a sign.
[[nodiscard]] double offsetFrom(const kernel::MeshSurface& surface, const Eigen::Vector3d& p);
/// The way that distance grows fastest.
[[nodiscard]] Eigen::Vector3d gradientOf(const kernel::MeshSurface& surface, const Eigen::Vector3d& p);
/// Which way the mesh faces at a point of a surface.
[[nodiscard]] Eigen::Vector3d facing(const kernel::MeshSurface& surface, const Eigen::Vector3d& p);

/// Where two surfaces of a mesh meet.
struct Contact {
    double length = 0.0;
    double tangent = 0.0; ///< how much of that they run smoothly into each other
};

class MeshFacts {
public:
    MeshFacts(const kernel::Mesh& mesh, const kernel::MeshSurfaces& found);

    [[nodiscard]] const std::vector<Eigen::Vector3d>& points() const { return m_points; }
    /// Across the box around the mesh.
    [[nodiscard]] double size() const { return m_size; }
    [[nodiscard]] double area() const { return m_area; }
    [[nodiscard]] const std::map<Index, Contact>& contacts(Index surface) const {
        return m_contacts[surface];
    }
    /// How far around its axis a surface goes.
    [[nodiscard]] double sweepOf(Index surface) const;
    /// The surfaces a surface runs smoothly into, the longest contact first.
    [[nodiscard]] std::vector<Index> tangentTo(Index surface) const;
    /// A round: a piece of a cylinder or a torus that runs smoothly into what is on either side.
    [[nodiscard]] bool isRound(Index surface) const;
    /// A bevel: a narrow flat or cone between two faces that are not tangent to it.
    [[nodiscard]] bool isBevel(Index surface) const;
    /// How wide a narrow surface is: its area over its longest side.
    [[nodiscard]] double widthOf(Index surface) const;
    /// The place on a surface nearest to its middle, which for a ring is not on it.
    [[nodiscard]] Eigen::Vector3d pointOn(Index surface) const;
    /// The place nearest to `from` that lies on all these surfaces, within `reach` of it. Nothing
    /// if they do not meet there.
    [[nodiscard]] std::optional<Eigen::Vector3d> meeting(const Eigen::Vector3d& from,
                                                         const std::vector<Index>& surfaces,
                                                         double reach) const;

private:
    const kernel::Mesh& m_mesh;
    const kernel::MeshSurfaces& m_found;
    std::vector<Eigen::Vector3d> m_points;
    double m_size = 0.0;
    double m_area = 0.0;
    std::vector<std::map<Index, Contact>> m_contacts;
};

} // namespace lcad::recover
