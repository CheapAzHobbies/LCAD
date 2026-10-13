// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

namespace lcad::app {

/// LCAD's own version, for example "0.0.1".
[[nodiscard]] QString versionString();

/// Where LCAD keeps its log: lcad/lcad.log under $XDG_STATE_HOME, or under ~/.local/state when
/// that is not set.
[[nodiscard]] QString defaultLogFilePath();

/// Starts writing LCAD's log to `filePath`, and warnings and errors to the terminal as well.
/// Messages from Qt and from the geometry kernel are routed into the same log. When the file
/// cannot be opened, logging goes to the terminal only.
///
/// A log file larger than 1 MiB is moved aside to <name>.1 first, replacing an older one.
void startLogging(const QString& filePath = defaultLogFilePath());

/// Undoes startLogging(): closes the file and gives Qt its own message handling back.
void stopLogging();

/// The file startLogging() is writing to, or an empty string.
[[nodiscard]] QString logFilePath();

} // namespace lcad::app
