// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Archive.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <vector>

namespace lcad::document::detail {

/// A document as stored: the two JSON files parsed, and every entry of the archive.
struct ArchiveContents {
    nlohmann::json manifest;
    nlohmann::json document;
    Entries entries;
};

/// Converts contents in format version N to version N + 1, in memory.
using Migration = std::function<void(ArchiveContents&)>;

/// LCAD's migrations: element 0 takes version 1 to version 2, element 1 takes 2 to 3, and so
/// on. Empty while version 1 is the only one.
[[nodiscard]] const std::vector<Migration>& migrations();

/// Brings `contents` from `fromVersion` up to `toVersion` by running the steps of `table` in
/// order, and records the new version in the manifest. Throws FileError when a step is missing.
void migrate(ArchiveContents& contents, int fromVersion, int toVersion,
             const std::vector<Migration>& table);

} // namespace lcad::document::detail
