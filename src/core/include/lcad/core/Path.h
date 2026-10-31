// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// File names as text. std::filesystem::path::string() is in the system's narrow encoding,
// which on Windows is a code page that cannot hold every name; LCAD's messages, its file
// format, and the libraries it hands file names to (libzip, Open CASCADE) all take UTF-8.
namespace lcad::core {

/// A file name in UTF-8, for messages and for libraries that take UTF-8 names.
[[nodiscard]] std::string toUtf8(const std::filesystem::path& path);
/// The same with forward slashes on every system, as links are stored.
[[nodiscard]] std::string toGenericUtf8(const std::filesystem::path& path);
/// A file name given in UTF-8.
[[nodiscard]] std::filesystem::path pathFromUtf8(std::string_view text);

} // namespace lcad::core
