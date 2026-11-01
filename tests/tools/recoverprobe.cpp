// SPDX-License-Identifier: GPL-3.0-or-later

// Tries turning meshes into features, on files named on the command line, or one per line on
// standard input, and prints a line for each. For judging changes against a pile of real meshes,
// which do not belong in the repository.

#include <lcad/document/Document.h>
#include <lcad/kernel/Error.h>
#include <lcad/kernel/Exchange.h>
#include <lcad/recover/Recover.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

using namespace lcad;

namespace {

const char* nameOf(recover::Result result) {
    switch (result) {
    case recover::Result::Features:
        return "features";
    case recover::Result::FeaturesOnSolid:
        return "onsolid";
    case recover::Result::ExactSolid:
        return "exact";
    case recover::Result::Facets:
        return "facets";
    case recover::Result::Failed:
        return "failed";
    }
    return "?";
}

void probe(const std::filesystem::path& path) {
    const auto start = std::chrono::steady_clock::now();
    try {
        const kernel::Mesh mesh = kernel::readMesh(path);
        document::Document model;
        const recover::MeshResult made = recover::addMesh(model, mesh, path.stem().string());
        const std::chrono::duration<double> taken = std::chrono::steady_clock::now() - start;
        std::printf("%-44.44s tris %7zu | %.2fs", path.filename().string().c_str(),
                    mesh.triangles.size(), taken.count());
        for (const recover::PartResult& part : made.parts) {
            std::printf(" | %s %.3f pulls %d holes %d fillets %d chamfers %d %s", nameOf(part.result),
                        part.recognized, part.pulls, part.holes, part.fillets, part.chamfers,
                        part.note.c_str());
        }
        std::printf("\n");
    } catch (const kernel::Error& failure) {
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
