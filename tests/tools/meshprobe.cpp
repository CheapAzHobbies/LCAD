// SPDX-License-Identifier: GPL-3.0-or-later

// Tries mesh recognition on files named on the command line, or one per line on standard input,
// and prints a line for each. For judging changes against a pile of real meshes, which do not
// belong in the repository.

#include <lcad/kernel/Check.h>
#include <lcad/kernel/Error.h>
#include <lcad/kernel/Exchange.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/MeshSolid.h>
#include <lcad/kernel/MeshSurfaces.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

using namespace lcad::kernel;

namespace {

void probe(const std::filesystem::path& path) {
    const auto start = std::chrono::steady_clock::now();
    try {
        const Mesh mesh = readMesh(path);
        const MeshSurfaces found = findSurfaces(mesh);
        std::array<int, 5> counts{};
        int facets = 0;
        for (const MeshSurface& s : found.surfaces) {
            if (s.facet) {
                ++facets;
            } else {
                ++counts[static_cast<std::size_t>(s.kind)];
            }
        }
        const std::chrono::duration<double> seeking = std::chrono::steady_clock::now() - start;
        const MeshSolids made = solidsFromMesh(mesh, found);
        double solidVolume = 0.0;
        int sound = 0;
        for (const Shape& solid : made.solids) {
            solidVolume += volume(solid);
            sound += checkGeometry(solid).empty() ? 1 : 0;
        }
        const std::chrono::duration<double> taken = std::chrono::steady_clock::now() - start;
        std::printf("%-40.40s tris %7zu  P%-4d C%-4d K%-3d S%-3d T%-3d facets %-5d share %.3f | "
                    "solids %d sound %d open %d faceted %d failed %d vol %.4f | %.2fs %.2fs\n",
                    path.filename().string().c_str(), mesh.triangles.size(), counts[0], counts[1],
                    counts[2], counts[3], counts[4], facets, found.share,
                    static_cast<int>(made.solids.size()), sound, made.openParts, made.facetedParts,
                    made.failedParts, solidVolume / meshVolume(mesh), seeking.count(), taken.count());
    } catch (const Error& failure) {
        std::printf("%-44.44s %s\n", path.filename().string().c_str(), failure.what());
    }
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            probe(argv[i]);
        }
        return 0;
    }
    std::string line;
    while (std::getline(std::cin, line)) {
        probe(line);
    }
    return 0;
}
