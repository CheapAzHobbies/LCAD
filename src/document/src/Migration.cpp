// SPDX-License-Identifier: GPL-3.0-or-later

#include "Migration.h"

#include <lcad/document/File.h>

#include <string>

namespace lcad::document::detail {

const std::vector<Migration>& migrations() {
    // When the format changes: add the function that converts the previous version here, raise
    // currentFormatVersion, and keep a sample file of the previous version in tests/models/.
    static const std::vector<Migration> table;
    return table;
}

void migrate(ArchiveContents& contents, int fromVersion, int toVersion,
             const std::vector<Migration>& table) {
    for (int version = fromVersion; version < toVersion; ++version) {
        const auto step = static_cast<std::size_t>(version - 1);
        if (version < 1 || step >= table.size() || !table[step]) {
            throw FileError("This LCAD cannot convert a document from format version " +
                            std::to_string(version) + " to " + std::to_string(version + 1) + ".");
        }
        table[step](contents);
    }
    contents.manifest["formatVersion"] = toVersion;
}

} // namespace lcad::document::detail
