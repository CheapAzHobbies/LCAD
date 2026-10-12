// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/Application.h>

namespace lcad::app {

QString versionString() {
    return QStringLiteral(LCAD_VERSION);
}

} // namespace lcad::app
