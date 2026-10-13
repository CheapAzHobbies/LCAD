// SPDX-License-Identifier: GPL-3.0-or-later

#include "Archive.h"
#include "ReferenceModelChecks.h"

#include <lcad/document/File.h>
#include <lcad/kernel/Version.h>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <set>
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

    // The oldest and the newest OCCT series that LCAD supports.
    CHECK(writers.contains("OCCT 7.6"));
}
