// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The zip container behind the .lcad format. Entry contents are bytes held in std::string.
namespace lcad::document::detail {

using Entries = std::map<std::string, std::string>;

/// The most uncompressed data readArchive() accepts from one file. A document this large is
/// not expected; the limit keeps a damaged or hostile file from exhausting memory.
inline constexpr std::uint64_t maximumArchiveBytes = std::uint64_t{4} << 30;

/// Every entry of the zip file at `path`. Throws FileError when the file is not a readable zip
/// archive, fails its checksums, or holds more than `byteLimit` bytes of data.
[[nodiscard]] Entries readArchive(const std::filesystem::path& path,
                                  std::uint64_t byteLimit = maximumArchiveBytes);

/// One entry of the zip file at `path`, or nothing when it has no entry of that name.
[[nodiscard]] std::optional<std::string> readArchiveEntry(const std::filesystem::path& path,
                                                          const std::string& name);

/// Creates a zip file at `path` with the given entries, in order. There must be no file at
/// `path` yet. Throws FileError on failure, leaving no file behind.
void writeArchive(const std::filesystem::path& path,
                  const std::vector<std::pair<std::string, std::string>>& entries);

} // namespace lcad::document::detail
