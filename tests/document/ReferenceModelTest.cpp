// SPDX-License-Identifier: GPL-3.0-or-later

#include "Archive.h"
#include "ReferenceModelChecks.h"
#include "ReferenceStep.h"

#include <lcad/document/File.h>
#include <lcad/kernel/Version.h>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

using namespace lcad::document;

// tests/models/format-1/ holds the same document written by LCAD built against different OCCT
// versions. Every build has to read all of them, which is what proves that a file saved on one
// distribution opens on another. See tests/models/README.md.
TEST_CASE("Reference documents written with each OCCT version open and measure correctly",
          "[document][file][models]") {
    const std::filesystem::path directory =
        std::filesystem::path(LCAD_TEST_MODELS_DIR) / "format-1";
    std::set<std::string> writers;

    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != ".lcad") {
            continue;
        }
        INFO(entry.path().filename().string()
             << ", read with OCCT " << lcad::kernel::occtVersion());
        const LoadedDocument loaded = loadDocument(entry.path());

        CHECK(loaded.formatVersion == 1);
        CHECK(loaded.warnings.empty());
        lcad::test::checkReferenceDocument(*loaded.document);

        const auto manifest = nlohmann::json::parse(
            lcad::document::detail::readArchiveEntry(entry.path(), "manifest.json").value());
        // "OCCT 7.6.3" becomes "OCCT 7.6".
        const std::string kernel = manifest["writtenBy"]["kernel"];
        writers.insert(kernel.substr(0, kernel.rfind('.')));
    }

    // The oldest and the newest OCCT series that LCAD supports. Whichever of them this build
    // uses, it has just read a file written by the other.
    CHECK(writers.contains("OCCT 7.6"));
    CHECK(writers.contains("OCCT 7.9"));
}

// tests/models/step/ holds the same STEP file written by LCAD built against different OCCT
// versions. Every build reads all of them: a STEP file LCAD writes on one distribution imports
// on another.
TEST_CASE("Reference STEP files written with each OCCT version import correctly",
          "[document][exchange][models]") {
    const std::filesystem::path directory = std::filesystem::path(LCAD_TEST_MODELS_DIR) / "step";
    std::set<std::string> writers;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != ".step") {
            continue;
        }
        INFO(entry.path().filename().string()
             << ", read with OCCT " << lcad::kernel::occtVersion());
        lcad::test::checkReferenceStep(lcad::kernel::importStep(entry.path()));
        // The header names the program that wrote it: "Open CASCADE STEP processor 7.6".
        std::ifstream in(entry.path());
        std::stringstream text;
        text << in.rdbuf();
        std::smatch processor;
        const std::string header = text.str().substr(0, 2000);
        if (std::regex_search(header, processor, std::regex(R"(STEP processor (\d+\.\d+))"))) {
            writers.insert("OCCT " + processor[1].str());
        }
    }
    // The oldest and the newest OCCT series that LCAD supports, as for the documents.
    CHECK(writers.contains("OCCT 7.6"));
    CHECK(writers.contains("OCCT 7.9"));
}
