// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/Theme.h>

#include <QApplication>
#include <QPalette>
#include <QProcess>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

namespace lcad::app {

namespace {

const char* const themeKey = "appearance/theme";

QString name(const QColor& color) {
    return color.name(QColor::HexRgb);
}

} // namespace

std::optional<bool> darkFromDesktopSetting(const QString& answer) {
    if (answer.contains(QLatin1String("prefer-dark"))) {
        return true;
    }
    if (answer.contains(QLatin1String("prefer-light")) ||
        answer.contains(QLatin1String("default"))) {
        return false;
    }
    return std::nullopt;
}

ThemeTokens lightTokens() {
    ThemeTokens t;
    t.dark = false;
    t.window = QColor(0xee, 0xf0, 0xf3);
    t.panel = QColor(0xf8, 0xf9, 0xfa);
    t.field = QColor(0xff, 0xff, 0xff);
    t.border = QColor(0xcd, 0xd2, 0xd9);
    t.text = QColor(0x1f, 0x23, 0x29);
    t.textSecondary = QColor(0x5d, 0x66, 0x73);
    t.textDisabled = QColor(0xa3, 0xaa, 0xb5);
    t.accent = QColor(0x1f, 0x6f, 0xeb);
    t.accentText = QColor(0xff, 0xff, 0xff);
    t.hover = QColor(0xe1, 0xe5, 0xea);
    t.warning = QColor(0xb3, 0x6b, 0x00);
    t.error = QColor(0xc9, 0x2a, 0x2a);
    t.success = QColor(0x2b, 0x8a, 0x3e);

    t.viewport.background = QColor(0xe4, 0xe7, 0xec);
    t.viewport.body = QColor(0x9d, 0xa8, 0xb6);
    t.viewport.bodyEdge = QColor(0x23, 0x27, 0x2e);
    t.viewport.line = QColor(0x23, 0x27, 0x2e);
    t.viewport.hover = QColor(0x0b, 0x0d, 0x10);
    t.viewport.selection = t.accent;
    t.viewport.preview = QColor(0x3d, 0x8b, 0xfd);
    t.viewport.grid = QColor(0xd3, 0xd7, 0xde);
    t.viewport.gridMajor = QColor(0xbd, 0xc3, 0xcc);
    t.viewport.cube = QColor(0xfb, 0xfc, 0xfd);
    t.viewport.cubeText = QColor(0x2a, 0x2f, 0x37);
    // A sketch: blue while it can still move, the color of text once it cannot.
    t.viewport.sketch = QColor(0x1b, 0x5f, 0xc4);
    t.viewport.sketchFree = QColor(0x1a, 0x73, 0xe8);
    t.viewport.sketchConstrained = QColor(0x1d, 0x21, 0x27);
    t.viewport.sketchReference = QColor(0x8e, 0x44, 0xc9);
    t.viewport.sketchConflict = t.error;
    t.viewport.sketchDimension = QColor(0x5b, 0x63, 0x70);
    t.viewport.label = QColor(0xfb, 0xfc, 0xfd);
    return t;
}

ThemeTokens darkTokens() {
    ThemeTokens t;
    t.dark = true;
    t.window = QColor(0x1e, 0x20, 0x24);
    t.panel = QColor(0x26, 0x29, 0x2e);
    t.field = QColor(0x1a, 0x1c, 0x20);
    t.border = QColor(0x3a, 0x3f, 0x47);
    t.text = QColor(0xe4, 0xe7, 0xec);
    t.textSecondary = QColor(0x9a, 0xa2, 0xae);
    t.textDisabled = QColor(0x5f, 0x66, 0x70);
    t.accent = QColor(0x4c, 0x94, 0xff);
    t.accentText = QColor(0x0d, 0x11, 0x17);
    t.hover = QColor(0x33, 0x37, 0x3e);
    t.warning = QColor(0xe0, 0xa1, 0x3a);
    t.error = QColor(0xf0, 0x6a, 0x6a);
    t.success = QColor(0x5c, 0xc0, 0x74);

    t.viewport.background = QColor(0x2e, 0x31, 0x36);
    t.viewport.body = QColor(0xa8, 0xb0, 0xba);
    t.viewport.bodyEdge = QColor(0x1e, 0x20, 0x24);
    t.viewport.line = QColor(0xec, 0xee, 0xf2);
    t.viewport.hover = QColor(0xff, 0xff, 0xff);
    t.viewport.selection = QColor(0x14, 0x78, 0xff);
    t.viewport.preview = QColor(0x5a, 0xaa, 0xff);
    t.viewport.grid = QColor(0x3d, 0x41, 0x48);
    t.viewport.gridMajor = QColor(0x4e, 0x53, 0x5c);
    t.viewport.cube = QColor(0xd6, 0xda, 0xe0);
    t.viewport.cubeText = QColor(0x28, 0x2c, 0x32);
    t.viewport.sketch = QColor(0x7c, 0xb7, 0xff);
    t.viewport.sketchFree = QColor(0x5a, 0xaa, 0xff);
    t.viewport.sketchConstrained = QColor(0xec, 0xee, 0xf2);
    t.viewport.sketchReference = QColor(0xc4, 0x8c, 0xf0);
    t.viewport.sketchConflict = t.error;
    t.viewport.sketchDimension = QColor(0xaa, 0xb0, 0xba);
    t.viewport.label = QColor(0x26, 0x28, 0x2d);
    return t;
}

Theme::Theme() : m_tokens(lightTokens()) {}

Theme& Theme::instance() {
    static Theme theme;
    return theme;
}

bool Theme::systemPrefersDark() const {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
    if (scheme != Qt::ColorScheme::Unknown) {
        return scheme == Qt::ColorScheme::Dark;
    }
#endif
    // Older Qt does not say. What it did give the application is a palette from the desktop,
    // and a dark desktop gives a dark one.
    return m_systemDark;
}

void Theme::load() {
    if (!m_systemProbed) {
        // Before the first apply() replaces it: the palette the desktop handed over.
        m_systemDark = QApplication::palette().color(QPalette::Window).lightness() < 128;
        m_systemProbed = true;
#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
        // This Qt cannot be asked, and its palette is light on a dark GNOME desktop. In a
        // desktop session, ask the desktop's settings tool instead. It is not there on every
        // desktop, and then the palette's answer stands.
        if (qEnvironmentVariableIsSet("XDG_CURRENT_DESKTOP")) {
            QProcess gsettings;
            gsettings.start(QStringLiteral("gsettings"),
                            {QStringLiteral("get"), QStringLiteral("org.gnome.desktop.interface"),
                             QStringLiteral("color-scheme")});
            if (gsettings.waitForFinished(1000) && gsettings.exitStatus() == QProcess::NormalExit &&
                gsettings.exitCode() == 0) {
                const auto dark =
                    darkFromDesktopSetting(QString::fromUtf8(gsettings.readAllStandardOutput()));
                m_systemDark = dark.value_or(m_systemDark);
            } else {
                gsettings.kill();
                gsettings.waitForFinished(200);
            }
        }
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
            if (m_choice == ThemeChoice::System) {
                apply();
            }
        });
#endif
    }
    const QString stored = QSettings().value(QLatin1String(themeKey)).toString();
    m_choice = stored == QLatin1String("light")  ? ThemeChoice::Light
               : stored == QLatin1String("dark") ? ThemeChoice::Dark
                                                 : ThemeChoice::System;
    apply();
}

void Theme::setChoice(ThemeChoice choice) {
    m_choice = choice;
    QSettings().setValue(QLatin1String(themeKey),
                         choice == ThemeChoice::Light  ? QStringLiteral("light")
                         : choice == ThemeChoice::Dark ? QStringLiteral("dark")
                                                       : QStringLiteral("system"));
    apply();
}

void Theme::apply() {
    const bool dark =
        m_choice == ThemeChoice::Dark || (m_choice == ThemeChoice::System && systemPrefersDark());
    m_tokens = dark ? darkTokens() : lightTokens();
    const ThemeTokens& t = m_tokens;

    // One widget style on every desktop, so that the tokens mean the same everywhere.
    if (QApplication::style()->objectName().compare(QLatin1String("fusion"), Qt::CaseInsensitive) !=
        0) {
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    }

    QPalette palette;
    palette.setColor(QPalette::Window, t.panel);
    palette.setColor(QPalette::WindowText, t.text);
    palette.setColor(QPalette::Base, t.field);
    palette.setColor(QPalette::AlternateBase, t.panel);
    palette.setColor(QPalette::Text, t.text);
    palette.setColor(QPalette::Button, t.panel);
    palette.setColor(QPalette::ButtonText, t.text);
    palette.setColor(QPalette::ToolTipBase, t.field);
    palette.setColor(QPalette::ToolTipText, t.text);
    palette.setColor(QPalette::PlaceholderText, t.textSecondary);
    palette.setColor(QPalette::Highlight, t.accent);
    palette.setColor(QPalette::HighlightedText, t.accentText);
    palette.setColor(QPalette::Link, t.accent);
    palette.setColor(QPalette::Mid, t.border);
    palette.setColor(QPalette::Dark, t.border);
    palette.setColor(QPalette::Light, t.field);
    palette.setColor(QPalette::Midlight, t.hover);
    palette.setColor(QPalette::Shadow, t.dark ? QColor(0, 0, 0) : t.border);
    for (const QPalette::ColorRole role :
         {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        palette.setColor(QPalette::Disabled, role, t.textDisabled);
    }
    QApplication::setPalette(palette);

    const QString sheet =
        QStringLiteral(R"(
        QMainWindow, QStatusBar { background: %1; }
        QWidget#topBar { background: %1; border-bottom: 1px solid %4; }
        QMenuBar { background: transparent; }
        QMenuBar::item { padding: 4px 8px; border-radius: %9px; background: transparent; }
        QMenuBar::item:selected { background: %7; }
        QMenu { background: %2; border: 1px solid %4; padding: 4px; }
        QMenu::item { padding: 5px 22px 5px 22px; border-radius: %9px; }
        QMenu::item:selected { background: %5; color: %6; }
        QMenu::item:disabled { color: %8; }
        QMenu::separator { height: 1px; background: %4; margin: 4px 6px; }
        QToolBar[tools="true"] { background: %2; border: none; border-bottom: 1px solid %4;
                                   padding: 2px 6px; spacing: 2px; min-height: %10px; }
        QToolBar[tools="true"] QToolButton, QWidget#topBar QToolButton {
            padding: 4px 8px; border-radius: %9px; border: 1px solid transparent; }
        QToolBar[tools="true"] QToolButton:hover, QWidget#topBar QToolButton:hover {
            background: %7; }
        QToolBar[tools="true"] QToolButton:pressed, QToolBar[tools="true"] QToolButton:checked {
            background: %7; border-color: %5; }
        QLabel#groupLabel { color: %3; font-weight: 600; padding: 0 4px 0 8px; }
        QLabel#secondary, QLabel#timeline { color: %3; }
        QDockWidget { titlebar-close-icon: none; }
        QDockWidget::title { background: %2; padding: 5px 8px; border-bottom: 1px solid %4; }
        QTabBar#documentTabs::tab { padding: 5px 12px; border: none;
                                    border-bottom: 2px solid transparent; color: %3; }
        QTabBar#documentTabs::tab:selected { color: %11; border-bottom-color: %5; }
        QTabBar#documentTabs::tab:hover { background: %7; }
        QFrame#commandSearch { background: %2; border: 1px solid %5; border-radius: %9px; }
        QFrame#commandSearch QLineEdit { border: none; padding: 6px 8px; background: %12; }
        QFrame#commandSearch QListWidget { border: none; background: %2; }
        QLabel#errorText { color: %13; }
        QToolBar#sketchToolbar QToolButton { padding: 4px 5px; }
        QToolBar#sketchToolbar QToolButton#constraintsButton { padding: 4px 18px 4px 8px; }
        QToolBar#sketchToolbar QToolButton#finishSketch {
            background: %5; color: %6; font-weight: 600; padding: 4px 12px;
            border-radius: %9px; }
        QLineEdit#sketchValue { background: %12; color: %11; border: 1px solid %5;
                                border-radius: %9px; padding: 2px 6px; }
        QLineEdit#sketchValue[invalid="true"] { border: 2px solid %13; }
        QLabel#sketchState { font-weight: 600; }
        QLabel#sketchState[state="free"] { color: %5; }
        QLabel#sketchState[state="conflict"] { color: %13; }
        QToolTip { background: %12; color: %11; border: 1px solid %4; padding: 3px 6px; }
    )")
            .arg(name(t.window), name(t.panel), name(t.textSecondary), name(t.border),
                 name(t.accent), name(t.accentText), name(t.hover), name(t.textDisabled))
            .arg(t.radius)
            .arg(t.toolbarHeight - 8)
            .arg(name(t.text), name(t.field), name(t.error));
    qApp->setStyleSheet(sheet);

    emit changed();
}

} // namespace lcad::app
