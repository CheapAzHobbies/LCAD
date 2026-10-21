// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

#include <filesystem>

// File names between Qt and the standard library, and where LCAD keeps its own files on each
// system.
namespace lcad::app {

/// A file name as the standard library takes it, whatever characters are in it.
[[nodiscard]] std::filesystem::path toPath(const QString& path);
/// A file name as Qt shows it.
[[nodiscard]] QString fromPath(const std::filesystem::path& path);

/// Where LCAD keeps its state (its log, copies of unsaved work): lcad under $XDG_STATE_HOME,
/// or ~/.local/state, on Linux; LCAD's local application data folder elsewhere
/// (%LOCALAPPDATA%\LCAD on Windows, ~/Library/Application Support/LCAD on macOS).
[[nodiscard]] QString stateFolder();

} // namespace lcad::app
