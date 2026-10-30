// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/Paths.h>

#include <QDir>
#include <QStandardPaths>

namespace lcad::app {

std::filesystem::path toPath(const QString& path) {
    // UTF-16 in, the system's own form out: wide on Windows, the file system's bytes elsewhere.
    return std::filesystem::path(path.toStdU16String());
}

QString fromPath(const std::filesystem::path& path) {
    return QString::fromStdU16String(path.u16string());
}

QString stateFolder() {
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    QString base = qEnvironmentVariable("XDG_STATE_HOME");
    if (base.isEmpty()) {
        base = QDir::homePath() + QStringLiteral("/.local/state");
    }
    return base + QStringLiteral("/lcad");
#else
    // %LOCALAPPDATA%\LCAD on Windows, ~/Library/Application Support/LCAD on macOS.
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty()) {
        base = QDir::homePath();
    }
    return base + QStringLiteral("/LCAD");
#endif
}

} // namespace lcad::app
