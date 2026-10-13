// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QIcon>
#include <QString>
#include <QStringList>

namespace lcad::app {

/// One of LCAD's own icons, by name: line drawings on a 24 unit grid with one stroke width,
/// painted in the theme's text color (the accent color where a detail is filled). An icon
/// follows the theme without being fetched again.
///
/// An unknown name gives an empty icon.
[[nodiscard]] QIcon icon(const QString& name);

/// The names icon() knows.
[[nodiscard]] QStringList iconNames();

} // namespace lcad::app
