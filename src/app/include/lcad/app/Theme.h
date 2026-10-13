// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/viewport/ViewportWidget.h>

#include <QColor>
#include <QObject>
#include <QString>

namespace lcad::app {

/// Which theme the user asked for.
enum class ThemeChoice { System, Light, Dark };

/// The design tokens: every color and size the interface uses is named here once, and the
/// palette, the style sheet, the icons, and the 3D view all take their values from this.
struct ThemeTokens {
    bool dark = false;

    // Surfaces
    QColor window; ///< The application background: top bar, status bar.
    QColor panel;  ///< Panels and toolbars.
    QColor field;  ///< Input boxes, lists, and trees.
    QColor border; ///< Separators and outlines.
    // Text
    QColor text;          ///< Primary text and icons.
    QColor textSecondary; ///< Group labels, hints, metadata.
    QColor textDisabled;
    // States
    QColor accent;     ///< The one accent color: selection, the active tool, primary buttons.
    QColor accentText; ///< Text on the accent color.
    QColor hover;      ///< Background of a control under the pointer.
    QColor warning;
    QColor error;
    QColor success;

    /// The 3D view's colors, coordinated with the rest.
    viewport::ViewportColors viewport;

    // Sizes, in logical pixels
    int radius = 4;         ///< Corner radius of buttons and fields.
    int spacing = 6;        ///< Standard gap between controls.
    int toolbarHeight = 36; ///< Height of the modeling toolbar.
    int iconSize = 18;      ///< Toolbar and menu icons.
    // Type sizes, in points, relative to the system's font size
    qreal titleScale = 1.0;      ///< Document titles and standard controls.
    qreal secondaryScale = 0.85; ///< Group labels and metadata.
};

/// The tokens of the light and of the dark theme.
[[nodiscard]] ThemeTokens lightTokens();
[[nodiscard]] ThemeTokens darkTokens();

/// The application's theme. One per process.
///
/// The choice (system, light, or dark) is remembered in the settings. "System" follows the
/// desktop's light or dark preference as far as Qt reports it.
class Theme : public QObject {
    Q_OBJECT

public:
    static Theme& instance();

    /// Reads the remembered choice and applies the theme to the application. Call once at
    /// startup, after QApplication exists.
    void load();

    [[nodiscard]] ThemeChoice choice() const { return m_choice; }
    /// Applies and remembers a choice.
    void setChoice(ThemeChoice choice);

    [[nodiscard]] const ThemeTokens& tokens() const { return m_tokens; }
    [[nodiscard]] bool isDark() const { return m_tokens.dark; }

signals:
    /// The tokens changed. Widgets that draw with them directly update themselves on this.
    void changed();

private:
    Theme();
    void apply();
    [[nodiscard]] bool systemPrefersDark() const;

    ThemeChoice m_choice = ThemeChoice::System;
    ThemeTokens m_tokens;
    bool m_systemDark = false;
    bool m_systemProbed = false;
};

} // namespace lcad::app
