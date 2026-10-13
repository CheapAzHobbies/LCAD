// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/BrowserPanel.h>
#include <lcad/app/CommandSearch.h>
#include <lcad/app/Commands.h>
#include <lcad/app/Icons.h>
#include <lcad/app/MainWindow.h>
#include <lcad/app/PropertiesPanel.h>
#include <lcad/app/ShortcutsDialog.h>
#include <lcad/app/Theme.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/viewport/ViewportWidget.h>

#include <FrameChecks.h>
#include <GraphicsMessages.h>
#include <GuiHelpers.h>

#include <QCheckBox>
#include <QDockWidget>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStatusBar>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>

#include <memory>

using lcad::app::CommandRegistry;
using lcad::app::MainWindow;
using lcad::app::PrimitiveKind;
using lcad::app::Theme;
using lcad::app::ThemeChoice;
using lcad::document::ObjectId;
using lcad::test::answerNextMessageBox;
using lcad::test::chooseFromNextMenu;
using lcad::test::differingFraction;
using lcad::test::drawnFraction;
using lcad::test::entriesOf;
using lcad::test::isNear;
using lcad::test::keepFrame;
using lcad::viewport::DisplayStyle;
using lcad::viewport::OriginItem;
using lcad::viewport::SelectedItem;
using lcad::viewport::StandardView;

namespace {

// Every action reachable through a menu, including those in submenus.
QSet<QAction*> actionsIn(const QMenu* menu) {
    QSet<QAction*> found;
    for (QAction* entry : menu->actions()) {
        if (entry->menu() != nullptr) {
            found.unite(actionsIn(entry->menu()));
        } else if (!entry->isSeparator()) {
            found.insert(entry);
        }
    }
    return found;
}

QSet<QAction*> actionsInMenus(const MainWindow& window) {
    QSet<QAction*> found;
    for (const QAction* top : window.mainMenu()->actions()) {
        found.unite(actionsIn(top->menu()));
    }
    return found;
}

// How far apart two colors are in brightness, from 0 to 255.
int contrast(const QColor& a, const QColor& b) {
    return qAbs(a.lightness() - b.lightness());
}

} // namespace

// The interface shell: the command registry and search, themes and icons, tabs, the remembered
// layout, the properties panel, context menus, and the viewport's display controls as the
// window drives them.
class InterfaceTest : public QObject {
    Q_OBJECT

public:
    static void initMain() {
        lcad::viewport::prepareApplication();
        // Keep the tests away from the real settings of whoever runs them, and from their
        // desktop's choice of a light or dark theme.
        qunsetenv("XDG_CURRENT_DESKTOP");
        static QTemporaryDir settingsDirectory;
        QCoreApplication::setOrganizationName(QStringLiteral("LCAD"));
        QCoreApplication::setApplicationName(QStringLiteral("LCAD"));
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDirectory.path());
    }

private slots:
    void init();
    void cleanup();

    // Commands
    void everyCommandIsDefinedOnceAndReachable();
    void searchFindsCommandsByNameAndAlias();
    void searchBoxRunsWhatIsChosen();
    void searchListsWhatCannotBeStartedLast();
    void shortcutsCanBeChangedAndAreRemembered();
    void shortcutsDialogRefusesAShortcutInUse();

    // Appearance
    void themesAreBuiltFromTokens_data();
    void themesAreBuiltFromTokens();
    void desktopPreferenceIsUnderstood();
    void themeChoiceAppliesEverywhereAndIsRemembered();
    void everyIconDrawsInBothThemes();
    void nothingEssentialIsClipped_data();
    void nothingEssentialIsClipped();

    // Documents
    void documentsOpenInTabs();
    void eachTabKeepsItsOwnViewAndHistory();
    void openingAFileUsesAnUntouchedTabAndNeverOpensTwice();
    void closingTabsAsksAboutUnsavedWork();
    void recentFilesAreRemembered();
    void statusBarSaysWhatHappened();

    // The window
    void layoutIsRememberedAndCanBeReset();
    void focusModeGivesTheWindowToTheView();

    // Panels and menus
    void propertiesShowAndEditTheSelectedBody();
    void propertiesMeasureFacesEdgesAndVertices();
    void browserFilterNarrowsTheList();
    void originGuidesBelongToTheirTab();
    void hideShowAndIsolateAreUndoable();
    void viewportMenuOffersWhatApplies();
    void viewportMenuReachesWhatLiesBehind();

    // The 3D view, driven from the window
    void gridAndDisplayStyleAreCommandsAndRemembered();

private:
    static void create(MainWindow& window, PrimitiveKind kind,
                       const QList<std::pair<QString, double>>& values = {});
    static QAction* action(const MainWindow& window, const char* id) {
        return window.commands()->action(QString::fromLatin1(id));
    }
    static void show(MainWindow& window) {
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
    }

    QTemporaryDir m_directory;
    lcad::test::GraphicsMessages m_graphicsMessages;
};

void InterfaceTest::init() {
    QTest::failOnWarning(QRegularExpression(QStringLiteral("OpenGL|QRhi|context|surface"),
                                            QRegularExpression::CaseInsensitiveOption));
    QSettings().clear();
    Theme::instance().load();
    QVERIFY(m_directory.isValid());
}

void InterfaceTest::cleanup() {
    const QStringList complaints = m_graphicsMessages.take();
    QVERIFY2(complaints.isEmpty(), qPrintable(complaints.join(QStringLiteral(" | "))));
}

void InterfaceTest::create(MainWindow& window, PrimitiveKind kind,
                           const QList<std::pair<QString, double>>& values) {
    window.startPrimitive(kind);
    for (const auto& [field, value] : values) {
        window.activePanel()->setValue(field, value);
    }
    window.acceptCommand();
}

// The acceptance test of this phase, first half: one definition per command, reached from the
// menus, the toolbar, shortcuts, and the search alike.
void InterfaceTest::everyCommandIsDefinedOnceAndReachable() {
    const MainWindow window;
    const CommandRegistry* registry = window.commands();
    const QStringList ids = registry->ids();
    QVERIFY(ids.size() >= 45);
    QCOMPARE(QSet<QString>(ids.begin(), ids.end()).size(), ids.size());

    const QSet<QAction*> inMenus = actionsInMenus(window);
    for (const QString& id : ids) {
        const lcad::app::Command* command = registry->command(id);
        QAction* its = registry->action(id);
        QVERIFY2(command != nullptr && its != nullptr, qPrintable(id));
        QVERIFY2(!command->description.isEmpty() && !command->group.isEmpty(), qPrintable(id));
        QVERIFY2(command->run != nullptr, qPrintable(id));
        // The same action object is what the menus hold: there is no second definition.
        QVERIFY2(inMenus.contains(its), qPrintable(id + QStringLiteral(" is in no menu")));
        // And typing its name finds it.
        bool found = false;
        for (const lcad::app::CommandMatch& match :
             registry->search(registry->displayName(id), 60)) {
            found = found || match.id == id;
        }
        QVERIFY2(found, qPrintable(id + QStringLiteral(" is not found by its own name")));
    }
    // Nothing is in a menu that the registry does not know, apart from the recent files.
    for (const QAction* entry : inMenus) {
        QVERIFY2(ids.contains(entry->objectName()) || entry->parent() != registry,
                 qPrintable(entry->text()));
    }

    // Toolbar buttons and the top bar's buttons are the registry's actions too.
    const auto* toolbar = window.findChild<QToolBar*>(QStringLiteral("modelingToolbar"));
    int toolbarCommands = 0;
    for (const QAction* entry : toolbar->actions()) {
        if (!entry->isSeparator() && !entry->objectName().isEmpty()) {
            QVERIFY2(ids.contains(entry->objectName()), qPrintable(entry->objectName()));
            ++toolbarCommands;
        }
    }
    QVERIFY(toolbarCommands >= 8);
    for (const QToolButton* button :
         window.findChild<QWidget*>(QStringLiteral("topBar"))
             ->findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly)) {
        QVERIFY(ids.contains(button->defaultAction()->objectName()));
    }

    // No two commands answer to the same keys.
    QHash<QString, QString> owners;
    for (const QString& id : ids) {
        for (const QKeySequence& sequence : registry->shortcuts(id)) {
            const QString keys = sequence.toString();
            QVERIFY2(!owners.contains(keys),
                     qPrintable(keys + QStringLiteral(": ") + owners.value(keys) +
                                QStringLiteral(" and ") + id));
            owners.insert(keys, id);
        }
    }
}

void InterfaceTest::searchFindsCommandsByNameAndAlias() {
    const MainWindow window;
    const CommandRegistry* registry = window.commands();
    const auto best = [registry](const char* typed) {
        const auto matches = registry->search(QString::fromLatin1(typed));
        return matches.isEmpty() ? QString() : matches.first().id;
    };

    // The start of a name, in any case.
    QCOMPARE(best("tor"), QStringLiteral("create.torus"));
    QCOMPARE(best("CYL"), QStringLiteral("create.cylinder"));
    QCOMPARE(best("wire"), QStringLiteral("view.style.wireframe"));
    // A later word of the name.
    QCOMPARE(best("layout"), QStringLiteral("window.resetLayout"));
    QCOMPARE(best("shortcuts"), QStringLiteral("window.shortcuts"));
    // Words the command is not called but might be looked for by.
    QCOMPARE(best("ball"), QStringLiteral("create.sphere"));
    QCOMPARE(best("donut"), QStringLiteral("create.torus"));
    QCOMPARE(best("cube"), QStringLiteral("create.box"));
    QCOMPARE(best("zoom all"), QStringLiteral("view.fit"));
    QCOMPARE(best("dark mode"), QStringLiteral("window.theme.dark"));
    QCOMPARE(best("exit"), QStringLiteral("file.quit"));
    // A name beats an alias: "box" is the Box, although other commands mention boxes.
    QCOMPARE(best("box"), QStringLiteral("create.box"));

    QVERIFY(registry->search(QStringLiteral("xyzzy")).isEmpty());
    QVERIFY(registry->search(QStringLiteral("   ")).isEmpty());
    QVERIFY(registry->search(QStringLiteral("e"), 5).size() <= 5);
}

void InterfaceTest::searchBoxRunsWhatIsChosen() {
    MainWindow window;
    show(window);
    lcad::app::CommandSearch* search = window.commandSearch();
    QVERIFY(!search->isVisible());

    // Opened by its own command. With nothing run yet there is nothing recent to list.
    QVERIFY(window.commands()->trigger(QStringLiteral("window.search")));
    QVERIFY(search->isVisible());
    QVERIFY(search->geometry().top() < window.height() / 3);
    QVERIFY(search->listedCommands().isEmpty());

    QTest::keyClicks(search->field(), QStringLiteral("cone"));
    QCOMPARE(search->listedCommands().first(), QStringLiteral("create.cone"));
    QTest::keyClick(search->field(), Qt::Key_Return);

    // The real command ran: its panel is open and the box has gone.
    QVERIFY(!search->isVisible());
    QVERIFY(window.activePanel() != nullptr);
    QCOMPARE(window.activePanel()->kind(), PrimitiveKind::Cone);
    window.acceptCommand();
    QCOMPARE(window.document()->bodies().size(), std::size_t{1});

    // The arrow keys choose among several, and what was run is offered first next time.
    window.commands()->trigger(QStringLiteral("window.search"));
    QCOMPARE(search->listedCommands(), QStringList{QStringLiteral("create.cone")});
    QTest::keyClicks(search->field(), QStringLiteral("view"));
    const QStringList listed = search->listedCommands();
    QVERIFY(listed.size() >= 3);
    QTest::keyClick(search->field(), Qt::Key_Down);
    QTest::keyClick(search->field(), Qt::Key_Down);
    QTest::keyClick(search->field(), Qt::Key_Up);
    QTest::keyClick(search->field(), Qt::Key_Return);
    QCOMPARE(window.commands()->recent().first(), listed.at(1));

    // Esc closes without running anything.
    window.commands()->trigger(QStringLiteral("window.search"));
    QTest::keyClicks(search->field(), QStringLiteral("torus"));
    QTest::keyClick(search->field(), Qt::Key_Escape);
    QVERIFY(!search->isVisible());
    QVERIFY(window.activePanel() == nullptr);
}

void InterfaceTest::searchListsWhatCannotBeStartedLast() {
    MainWindow window;
    show(window);
    create(window, PrimitiveKind::Box);
    lcad::app::CommandSearch* search = window.commandSearch();

    // Nothing is selected, so Delete cannot be started. It is listed, and Enter leaves it be.
    window.commands()->trigger(QStringLiteral("window.search"));
    QTest::keyClicks(search->field(), QStringLiteral("delete"));
    QVERIFY(search->listedCommands().contains(QStringLiteral("edit.delete")));
    QTest::keyClick(search->field(), Qt::Key_Return);
    QCOMPARE(window.document()->bodies().size(), std::size_t{1});
    QTest::keyClick(search->field(), Qt::Key_Escape);

    // With the body selected it is the first answer, and Enter deletes.
    window.browser()->bodyItem(window.document()->bodies()[0].id)->setSelected(true);
    window.commands()->trigger(QStringLiteral("window.search"));
    QTest::keyClicks(search->field(), QStringLiteral("delete"));
    QCOMPARE(search->listedCommands().first(), QStringLiteral("edit.delete"));
    QTest::keyClick(search->field(), Qt::Key_Return);
    QVERIFY(window.document()->bodies().empty());
}

void InterfaceTest::shortcutsCanBeChangedAndAreRemembered() {
    const QKeySequence f8(Qt::Key_F8);
    {
        const MainWindow window;
        CommandRegistry* registry = window.commands();
        QCOMPARE(registry->shortcuts(QStringLiteral("view.fit")),
                 QList<QKeySequence>{QKeySequence(Qt::Key_F6)});

        QVERIFY(registry->setShortcuts(QStringLiteral("view.fit"), {f8}));
        QCOMPARE(action(window, "view.fit")->shortcut(), f8);
        // Keys that belong to another command are refused, and nothing changes.
        QVERIFY(!registry->setShortcuts(QStringLiteral("view.home"), {f8}));
        QCOMPARE(registry->commandWithShortcut(f8), QStringLiteral("view.fit"));
        QCOMPARE(action(window, "view.home")->shortcut(), QKeySequence(Qt::Key_Home));
        // A command can be left without any.
        QVERIFY(registry->setShortcuts(QStringLiteral("edit.delete"), {}));
    }
    {
        // As after a restart.
        const MainWindow window;
        QCOMPARE(action(window, "view.fit")->shortcut(), f8);
        QVERIFY(action(window, "edit.delete")->shortcuts().isEmpty());
        QCOMPARE(action(window, "file.save")->shortcut(), QKeySequence(QKeySequence::Save));

        window.commands()->resetShortcuts();
        QCOMPARE(action(window, "view.fit")->shortcut(), QKeySequence(Qt::Key_F6));
        QCOMPARE(action(window, "edit.delete")->shortcut(), QKeySequence(QKeySequence::Delete));
    }
    const MainWindow window;
    QCOMPARE(action(window, "view.fit")->shortcut(), QKeySequence(Qt::Key_F6));
}

void InterfaceTest::shortcutsDialogRefusesAShortcutInUse() {
    const MainWindow window;
    lcad::app::ShortcutsDialog dialog(window.commands(), nullptr);
    QKeySequenceEdit* fit = dialog.editorFor(QStringLiteral("view.fit"));
    QKeySequenceEdit* torus = dialog.editorFor(QStringLiteral("create.torus"));
    QVERIFY(fit != nullptr && torus != nullptr);
    QCOMPARE(fit->keySequence(), QKeySequence(Qt::Key_F6));
    QVERIFY(torus->keySequence().isEmpty());

    // Give Torus a shortcut of its own.
    torus->setKeySequence(QKeySequence(Qt::Key_T));
    emit torus->editingFinished();
    QVERIFY(dialog.message().isEmpty());
    QCOMPARE(action(window, "create.torus")->shortcut(), QKeySequence(Qt::Key_T));

    // Then try to give it Save's.
    torus->setKeySequence(QKeySequence(QKeySequence::Save));
    emit torus->editingFinished();
    QVERIFY2(dialog.message().contains(QStringLiteral("already belongs to \"Save\"")),
             qPrintable(dialog.message()));
    QCOMPARE(torus->keySequence(), QKeySequence(Qt::Key_T));
    QCOMPARE(action(window, "file.save")->shortcut(), QKeySequence(QKeySequence::Save));

    dialog.findChild<QPushButton*>(QStringLiteral("restoreDefaults"))->click();
    QVERIFY(action(window, "create.torus")->shortcuts().isEmpty());
    QVERIFY(dialog.editorFor(QStringLiteral("create.torus"))->keySequence().isEmpty());
}

void InterfaceTest::themesAreBuiltFromTokens_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("light") << false;
    QTest::newRow("dark") << true;
}

void InterfaceTest::themesAreBuiltFromTokens() {
    QFETCH(bool, dark);
    const lcad::app::ThemeTokens t = dark ? lcad::app::darkTokens() : lcad::app::lightTokens();

    QCOMPARE(t.dark, dark);
    // Text can be read on every surface it is put on.
    for (const QColor& surface : {t.window, t.panel, t.field}) {
        QVERIFY(contrast(t.text, surface) > 140);
        QVERIFY(contrast(t.textSecondary, surface) > 70);
        QVERIFY(contrast(t.textDisabled, surface) > 25);
    }
    QVERIFY(contrast(t.accentText, t.accent) > 90);
    // Surfaces differ from each other only a little; borders are what separates panels.
    QVERIFY(contrast(t.window, t.panel) < 30);
    QVERIFY(contrast(t.border, t.panel) > 15);
    // In the 3D view, a body stands out from the background and its edges from the body.
    QVERIFY(contrast(t.viewport.body, t.viewport.background) > 60);
    QVERIFY(contrast(t.viewport.bodyEdge, t.viewport.body) > 90);
    QVERIFY(contrast(t.viewport.line, t.viewport.background) > 140);
    QVERIFY(contrast(t.viewport.grid, t.viewport.background) < 40);
    QVERIFY(contrast(t.viewport.cubeText, t.viewport.cube) > 120);
    // One accent, the same hue family as the selection in the 3D view.
    QVERIFY(qAbs(t.accent.hue() - t.viewport.selection.hue()) < 15);
    // Errors are not the accent color.
    QVERIFY(qAbs(t.error.hue() - t.accent.hue()) > 60);
}

void InterfaceTest::desktopPreferenceIsUnderstood() {
    using lcad::app::darkFromDesktopSetting;
    // What `gsettings get org.gnome.desktop.interface color-scheme` prints.
    QCOMPARE(darkFromDesktopSetting(QStringLiteral("'prefer-dark'\n")), std::optional<bool>(true));
    QCOMPARE(darkFromDesktopSetting(QStringLiteral("'default'\n")), std::optional<bool>(false));
    QCOMPARE(darkFromDesktopSetting(QStringLiteral("'prefer-light'\n")),
             std::optional<bool>(false));
    QCOMPARE(darkFromDesktopSetting(QStringLiteral("No such schema")), std::optional<bool>());
    QCOMPARE(darkFromDesktopSetting(QString()), std::optional<bool>());
}

void InterfaceTest::themeChoiceAppliesEverywhereAndIsRemembered() {
    {
        MainWindow window;
        show(window);
        create(window, PrimitiveKind::Box);
        QCOMPARE(Theme::instance().choice(), ThemeChoice::System);
        QVERIFY(action(window, "window.theme.system")->isChecked());

        window.commands()->trigger(QStringLiteral("window.theme.dark"));
        QVERIFY(Theme::instance().isDark());
        QVERIFY(action(window, "window.theme.dark")->isChecked());
        QVERIFY(!action(window, "window.theme.system")->isChecked());
        const lcad::app::ThemeTokens dark = lcad::app::darkTokens();
        // Panels, the 3D view, and what the widgets draw with all changed together.
        QCOMPARE(window.viewport()->colors(), dark.viewport);
        QCOMPARE(QApplication::palette().color(QPalette::Window), dark.panel);
        QCOMPARE(QApplication::palette().color(QPalette::WindowText), dark.text);
        QCOMPARE(QApplication::palette().color(QPalette::Highlight), dark.accent);
        const QImage darkFrame = window.viewport()->grabFramebuffer();
        QVERIFY(isNear(darkFrame.pixel(2, darkFrame.height() / 2), dark.viewport.background));
        const QImage darkWindow = window.grab().toImage();
        keepFrame(darkWindow, "interface-dark");
        QVERIFY(QColor(darkWindow.pixel(8, window.height() / 2)).lightness() < 90);

        window.commands()->trigger(QStringLiteral("window.theme.light"));
        const lcad::app::ThemeTokens light = lcad::app::lightTokens();
        QVERIFY(!Theme::instance().isDark());
        QCOMPARE(window.viewport()->colors(), light.viewport);
        const QImage lightFrame = window.viewport()->grabFramebuffer();
        QVERIFY(isNear(lightFrame.pixel(2, lightFrame.height() / 2), light.viewport.background));
        // The box is drawn in both, against either background.
        QVERIFY(drawnFraction(lightFrame, light.viewport.background) > 0.10);
        QVERIFY(drawnFraction(darkFrame, dark.viewport.background) > 0.10);
        const QImage lightWindow = window.grab().toImage();
        keepFrame(lightWindow, "interface-light");
        QVERIFY(QColor(lightWindow.pixel(8, window.height() / 2)).lightness() > 180);

        window.commands()->trigger(QStringLiteral("window.theme.dark"));
    }
    // As after a restart: the choice is still dark.
    Theme::instance().load();
    QCOMPARE(Theme::instance().choice(), ThemeChoice::Dark);
    const MainWindow window;
    QVERIFY(Theme::instance().isDark());
    QCOMPARE(window.viewport()->colors(), lcad::app::darkTokens().viewport);
    QVERIFY(action(window, "window.theme.dark")->isChecked());
}

void InterfaceTest::everyIconDrawsInBothThemes() {
    const MainWindow window;
    const QStringList names = lcad::app::iconNames();
    QVERIFY(names.size() >= 30);
    QVERIFY(lcad::app::icon(QStringLiteral("no-such-icon")).isNull());

    // Every command that names an icon names one that exists.
    for (const QString& id : window.commands()->ids()) {
        const QString name = window.commands()->command(id)->iconName;
        QVERIFY2(name.isEmpty() || names.contains(name), qPrintable(id + QLatin1Char(' ') + name));
    }

    for (const ThemeChoice choice : {ThemeChoice::Light, ThemeChoice::Dark}) {
        Theme::instance().setChoice(choice);
        const QColor text = Theme::instance().tokens().text;
        for (const QString& name : names) {
            const QImage normal =
                lcad::app::icon(name).pixmap(QSize(48, 48), QIcon::Normal).toImage();
            const QImage disabled =
                lcad::app::icon(name).pixmap(QSize(48, 48), QIcon::Disabled).toImage();
            int inked = 0;
            int inTextColor = 0;
            for (int y = 0; y < normal.height(); ++y) {
                for (int x = 0; x < normal.width(); ++x) {
                    const QColor pixel = normal.pixelColor(x, y);
                    if (pixel.alpha() > 200) {
                        ++inked;
                        if (contrast(pixel, text) < 12) {
                            ++inTextColor;
                        }
                    }
                }
            }
            // Something is drawn, it does not fill the square, and it is drawn in the
            // theme's text color. In shares of the picture, which is larger on a high-DPI
            // screen.
            const double area = normal.width() * normal.height();
            QVERIFY2(inked > area * 0.015 && inked < area * 0.7, qPrintable(name));
            QVERIFY2(inTextColor > area * 0.008, qPrintable(name));
            QVERIFY2(normal != disabled, qPrintable(name));
        }
    }
}

void InterfaceTest::nothingEssentialIsClipped_data() {
    QTest::addColumn<bool>("dark");
    QTest::addColumn<QSize>("size");
    QTest::newRow("light 1280x800") << false << QSize(1280, 800);
    QTest::newRow("dark 1280x800") << true << QSize(1280, 800);
    QTest::newRow("light 1024x700") << false << QSize(1024, 700);
    QTest::newRow("dark 1600x1000") << true << QSize(1600, 1000);
}

// The acceptance test of this phase, second half: the interface holds together in both themes
// and at more than one window size. Run again at twice the scale by CTest.
void InterfaceTest::nothingEssentialIsClipped() {
    QFETCH(bool, dark);
    QFETCH(QSize, size);
    Theme::instance().setChoice(dark ? ThemeChoice::Dark : ThemeChoice::Light);
    MainWindow window;
    window.resize(size);
    show(window);
    create(window, PrimitiveKind::Box);
    window.browser()->bodyItem(window.document()->bodies()[0].id)->setSelected(true);
    QTRY_COMPARE(window.size(), size);
    // The properties of the selected body have been laid out and are on screen.
    QTRY_VERIFY(
        window.properties()->findChild<QLineEdit*>(QStringLiteral("bodyName")) != nullptr &&
        window.properties()->findChild<QLineEdit*>(QStringLiteral("bodyName"))->isVisible());
    keepFrame(window.grab().toImage(), QTest::currentDataTag());

    // Every toolbar button is shown whole, inside the toolbar, without an overflow arrow.
    const auto* toolbar = window.findChild<QToolBar*>(QStringLiteral("modelingToolbar"));
    QVERIFY(
        toolbar->findChild<QToolButton*>(QStringLiteral("qt_toolbar_ext_button")) == nullptr ||
        !toolbar->findChild<QToolButton*>(QStringLiteral("qt_toolbar_ext_button"))->isVisible());
    QList<QRect> placed;
    for (const QAction* entry : toolbar->actions()) {
        const QWidget* widget = toolbar->widgetForAction(const_cast<QAction*>(entry));
        if (widget == nullptr || entry->isSeparator()) {
            continue;
        }
        QVERIFY2(widget->isVisible(), qPrintable(entry->objectName()));
        const QRect where = widget->geometry();
        QVERIFY2(toolbar->rect().contains(where), qPrintable(entry->objectName()));
        // Wide enough for what it shows, and not on top of its neighbor.
        QVERIFY2(where.width() >= widget->minimumSizeHint().width(),
                 qPrintable(entry->objectName()));
        for (const QRect& other : placed) {
            QVERIFY2(!where.intersects(other), qPrintable(entry->objectName()));
        }
        placed << where;
    }

    // The top bar: menus, buttons, tabs, and search all fit side by side.
    const auto* topBar = window.findChild<QWidget*>(QStringLiteral("topBar"));
    QList<QRect> topPlaced;
    for (const QWidget* child :
         topBar->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (!child->isVisible()) {
            continue;
        }
        QVERIFY(topBar->rect().contains(child->geometry()));
        for (const QRect& other : topPlaced) {
            QVERIFY(!child->geometry().intersects(other));
        }
        topPlaced << child->geometry();
    }
    QVERIFY(window.mainMenu()->width() >= window.mainMenu()->sizeHint().width());

    // The panels do not overlap the 3D view, and the view keeps most of the window.
    const QRect view = window.viewport()->geometry();
    for (const char* name : {"browserDock", "propertiesDock", "timelineDock"}) {
        const auto* dock = window.findChild<QDockWidget*>(QString::fromLatin1(name));
        QVERIFY2(dock->isVisible() && !dock->geometry().intersects(view), name);
    }
    QVERIFY(view.width() > size.width() * 0.45);
    QVERIFY(view.height() > size.height() * 0.6);

    // The properties of the selected body are all on screen, inside their panel, each with
    // room for all of its text.
    const auto* propertiesDock = window.findChild<QDockWidget*>(QStringLiteral("propertiesDock"));
    int labelsShown = 0;
    for (const QLabel* label : window.properties()->findChildren<QLabel*>()) {
        QVERIFY2(label->isVisible(), qPrintable(label->text()));
        const QRect inDock(label->mapTo(propertiesDock, QPoint(0, 0)), label->size());
        QVERIFY2(propertiesDock->rect().contains(inDock), qPrintable(label->text()));
        if (label->wordWrap()) {
            QVERIFY2(label->heightForWidth(label->width()) <= label->height() + 1,
                     qPrintable(label->text()));
        } else {
            QVERIFY2(label->sizeHint().width() <= label->width(), qPrintable(label->text()));
        }
        ++labelsShown;
    }
    // The heading, the name's label, and a label and a value for each of the five measurements.
    QCOMPARE(labelsShown, 12);
    const auto* visibleBox =
        window.properties()->findChild<QCheckBox*>(QStringLiteral("bodyVisible"));
    QVERIFY(visibleBox->isVisible() && visibleBox->sizeHint().width() <= visibleBox->width());
    QVERIFY(window.properties()->rows().contains(QStringLiteral("Volume: 125000 mm³")));

    // One close button per tab, in LCAD's own style.
    const auto* tabs = window.findChild<QTabBar*>(QStringLiteral("documentTabs"));
    QCOMPARE(tabs->findChildren<QToolButton*>(QStringLiteral("closeTab")).size(), 1);
}

void InterfaceTest::documentsOpenInTabs() {
    MainWindow window;
    auto* tabs = window.findChild<QTabBar*>(QStringLiteral("documentTabs"));
    QCOMPARE(window.documentCount(), 1);
    QCOMPARE(tabs->count(), 1);

    create(window, PrimitiveKind::Box);
    // Unsaved work is marked in the tab's text.
    QCOMPARE(tabs->tabText(0), QStringLiteral("Untitled *"));
    const auto* first = window.document();

    window.commands()->trigger(QStringLiteral("file.new"));
    QCOMPARE(window.documentCount(), 2);
    QCOMPARE(tabs->count(), 2);
    QCOMPARE(window.currentIndex(), 1);
    QCOMPARE(tabs->currentIndex(), 1);
    QVERIFY(window.document() != first);
    QVERIFY(window.document()->bodies().empty());
    QCOMPARE(tabs->tabText(1), QStringLiteral("Untitled"));
    QCOMPARE(window.browser()->tree()->topLevelItem(0)->child(1)->childCount(), 0);
    QVERIFY(!window.isWindowModified());

    // Clicking a tab shows that document everywhere.
    tabs->setCurrentIndex(0);
    QCOMPARE(window.currentIndex(), 0);
    QCOMPARE(window.document(), first);
    QCOMPARE(window.browser()->tree()->topLevelItem(0)->child(1)->childCount(), 1);
    QVERIFY(window.isWindowModified());
    QCOMPARE(window.windowTitle(), QStringLiteral("Untitled[*] - LCAD"));

    // Saving names the tab and clears its mark.
    QVERIFY(window.saveTo(m_directory.filePath(QStringLiteral("plate.lcad"))));
    QCOMPARE(tabs->tabText(0), QStringLiteral("plate"));
    QVERIFY(tabs->tabToolTip(0).endsWith(QStringLiteral("plate.lcad")));
    QCOMPARE(window.documentName(0), QStringLiteral("plate"));
    QCOMPARE(window.documentName(1), QStringLiteral("Untitled"));
}

void InterfaceTest::eachTabKeepsItsOwnViewAndHistory() {
    MainWindow window;
    show(window);
    create(window, PrimitiveKind::Box);
    window.viewport()->setStandardView(StandardView::Top);
    QTest::mouseMove(window.viewport(), QPoint(4, 4));
    const QImage firstView = window.viewport()->grabFramebuffer();

    window.newDocument();
    create(window, PrimitiveKind::Sphere);
    create(window, PrimitiveKind::Cone);
    const QImage secondView = window.viewport()->grabFramebuffer();
    QVERIFY(differingFraction(firstView, secondView) > 0.02);
    QCOMPARE(QString::fromStdString(window.document()->undoLabel()), QStringLiteral("Create Cone"));

    // Back to the first: its camera, its one body, its own undo step.
    window.setCurrentIndex(0);
    QCOMPARE(window.document()->bodies().size(), std::size_t{1});
    QCOMPARE(action(window, "edit.undo")->text().remove(QLatin1Char('&')),
             QStringLiteral("Undo Create Box"));
    QTRY_VERIFY(differingFraction(firstView, window.viewport()->grabFramebuffer()) < 0.002);
    QVERIFY((window.viewport()->viewDirection() - QVector3D(0, 0, -1)).length() < 1e-3F);

    window.setCurrentIndex(1);
    QCOMPARE(window.document()->bodies().size(), std::size_t{2});
    QTRY_VERIFY(differingFraction(secondView, window.viewport()->grabFramebuffer()) < 0.002);

    // A command in progress belongs to the document it was started in.
    window.startPrimitive(PrimitiveKind::Torus);
    window.setCurrentIndex(0);
    QVERIFY(window.activePanel() == nullptr);
    QCOMPARE(window.document()->bodies().size(), std::size_t{1});
}

void InterfaceTest::openingAFileUsesAnUntouchedTabAndNeverOpensTwice() {
    const QString plate = m_directory.filePath(QStringLiteral("plate.lcad"));
    const QString peg = m_directory.filePath(QStringLiteral("peg.lcad"));
    {
        MainWindow writer;
        create(writer, PrimitiveKind::Box);
        QVERIFY(writer.saveTo(plate));
        writer.newDocument();
        create(writer, PrimitiveKind::Cylinder);
        QVERIFY(writer.saveTo(peg));
    }
    MainWindow window;
    QCOMPARE(window.documentCount(), 1);

    // The empty starting document makes way.
    QVERIFY(window.loadFrom(plate));
    QCOMPARE(window.documentCount(), 1);
    QCOMPARE(window.documentName(), QStringLiteral("plate"));

    // A second file gets a tab of its own.
    QVERIFY(window.loadFrom(peg));
    QCOMPARE(window.documentCount(), 2);
    QCOMPARE(window.currentIndex(), 1);
    QCOMPARE(window.document()->bodies()[0].name, std::string("Cylinder"));

    // Opening the first again shows its tab.
    const auto* pegDocument = window.document();
    QVERIFY(window.loadFrom(plate));
    QCOMPARE(window.documentCount(), 2);
    QCOMPARE(window.currentIndex(), 0);
    QVERIFY(window.document() != pegDocument);

    // A document that has been worked in is not replaced, even if it has no file.
    window.newDocument();
    create(window, PrimitiveKind::Sphere);
    QVERIFY(window.loadFrom(peg));
    QCOMPARE(window.documentCount(), 3);
    QCOMPARE(window.currentIndex(), 1);

    // A file that cannot be opened changes nothing.
    QString error;
    QVERIFY(!window.loadFrom(m_directory.filePath(QStringLiteral("missing.lcad")), &error));
    QCOMPARE(window.documentCount(), 3);
    QCOMPARE(window.currentIndex(), 1);
}

void InterfaceTest::closingTabsAsksAboutUnsavedWork() {
    MainWindow window;
    show(window);
    create(window, PrimitiveKind::Box);
    window.newDocument();
    window.newDocument();
    create(window, PrimitiveKind::Sphere);
    QCOMPARE(window.documentCount(), 3);

    // The untouched middle one closes without a question, from the button on its tab.
    auto* tabs = window.findChild<QTabBar*>(QStringLiteral("documentTabs"));
    qobject_cast<QToolButton*>(tabs->tabButton(1, QTabBar::RightSide))->click();
    QCOMPARE(window.documentCount(), 2);
    QCOMPARE(tabs->count(), 2);

    // The last one has unsaved work: Cancel keeps it, and it is the one on screen.
    QString asked;
    answerNextMessageBox(QMessageBox::Cancel, &asked);
    QVERIFY(!window.fileClose(1));
    QCOMPARE(asked, QStringLiteral("Save the changes to \"Untitled\" before closing it?"));
    QCOMPARE(window.documentCount(), 2);
    QCOMPARE(window.currentIndex(), 1);

    answerNextMessageBox(QMessageBox::Discard);
    QVERIFY(window.fileClose(1));
    QCOMPARE(window.documentCount(), 1);
    QCOMPARE(window.document()->bodies()[0].name, std::string("Box"));

    // Closing the only document leaves a fresh one: LCAD always has a document.
    answerNextMessageBox(QMessageBox::Discard);
    QVERIFY(window.commands()->trigger(QStringLiteral("file.close")));
    QCOMPARE(window.documentCount(), 1);
    QVERIFY(window.document()->bodies().empty());
    QVERIFY(!window.isWindowModified());

    // Closing the window asks about each document with unsaved work, in turn.
    create(window, PrimitiveKind::Cone);
    window.newDocument();
    create(window, PrimitiveKind::Torus);
    window.newDocument();
    answerNextMessageBox(QMessageBox::Discard);
    QTimer::singleShot(200, [] { answerNextMessageBox(QMessageBox::Cancel); });
    QVERIFY(!window.close());
    // The second question was about the second document, which is now showing.
    QCOMPARE(window.currentIndex(), 1);
    QVERIFY(window.isVisible());

    answerNextMessageBox(QMessageBox::Discard);
    QTimer::singleShot(200, [] { answerNextMessageBox(QMessageBox::Discard); });
    QVERIFY(window.close());
}

void InterfaceTest::recentFilesAreRemembered() {
    const QString plate = m_directory.filePath(QStringLiteral("plate.lcad"));
    const QString peg = m_directory.filePath(QStringLiteral("peg.lcad"));
    {
        MainWindow window;
        QVERIFY(window.recentFiles().isEmpty());
        create(window, PrimitiveKind::Box);
        QVERIFY(window.saveTo(plate));
        window.newDocument();
        create(window, PrimitiveKind::Cylinder);
        QVERIFY(window.saveTo(peg));
        QCOMPARE(window.recentFiles(), (QStringList{peg, plate}));
    }
    MainWindow window;
    QCOMPARE(window.recentFiles(), (QStringList{peg, plate}));
    // Opening one moves it to the front, without listing it twice.
    QVERIFY(window.loadFrom(plate));
    QCOMPARE(window.recentFiles(), (QStringList{plate, peg}));

    // The File menu offers them, and choosing one opens it.
    QMenu* recent = nullptr;
    for (QAction* entry : window.mainMenu()->actions().first()->menu()->actions()) {
        if (entry->menu() != nullptr) {
            recent = entry->menu();
        }
    }
    QVERIFY(recent != nullptr);
    QCOMPARE(recent->actions().size(), 2);
    QVERIFY(recent->actions().at(1)->text().startsWith(QStringLiteral("peg.lcad")));
    recent->actions().at(1)->trigger();
    QCOMPARE(window.documentName(), QStringLiteral("peg"));
    QCOMPARE(window.documentCount(), 2);
}

void InterfaceTest::statusBarSaysWhatHappened() {
    MainWindow window;
    show(window);
    auto* command = window.findChild<QLabel*>(QStringLiteral("activeCommand"));
    QVERIFY(command->text().isEmpty());

    window.startPrimitive(PrimitiveKind::Box);
    QCOMPARE(command->text(), QStringLiteral("Create Box"));
    window.acceptCommand();
    QVERIFY(command->text().isEmpty());

    // Saving is reported in passing, not with a dialog.
    const QString path = m_directory.filePath(QStringLiteral("bracket.lcad"));
    QVERIFY(window.saveTo(path));
    QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("Saved bracket"));
    QVERIFY(QApplication::activeModalWidget() == nullptr);

    window.closeDocument(0);
    QVERIFY(window.loadFrom(path));
    QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("Opened bracket"));
}

void InterfaceTest::layoutIsRememberedAndCanBeReset() {
    {
        MainWindow window;
        show(window);
        auto* browser = window.findChild<QDockWidget*>(QStringLiteral("browserDock"));
        auto* timeline = window.findChild<QDockWidget*>(QStringLiteral("timelineDock"));
        QVERIFY(browser->isVisible() && timeline->isVisible());
        QVERIFY(action(window, "window.browser")->isChecked());

        // Hide the browser, collapse the timeline, move the properties to the left.
        window.commands()->trigger(QStringLiteral("window.browser"));
        window.commands()->trigger(QStringLiteral("window.timeline"));
        QVERIFY(!browser->isVisible() && !timeline->isVisible());
        QVERIFY(!action(window, "window.browser")->isChecked());
        window.addDockWidget(Qt::LeftDockWidgetArea,
                             window.findChild<QDockWidget*>(QStringLiteral("propertiesDock")));
        window.resize(1100, 720);
        QVERIFY(window.close());
    }
    // As after a restart: the window comes back as it was left.
    MainWindow window;
    show(window);
    auto* browser = window.findChild<QDockWidget*>(QStringLiteral("browserDock"));
    auto* timeline = window.findChild<QDockWidget*>(QStringLiteral("timelineDock"));
    auto* properties = window.findChild<QDockWidget*>(QStringLiteral("propertiesDock"));
    QVERIFY(!browser->isVisible());
    QVERIFY(!timeline->isVisible());
    QCOMPARE(window.dockWidgetArea(properties), Qt::LeftDockWidgetArea);
    QCOMPARE(window.size(), QSize(1100, 720));
    QVERIFY(!action(window, "window.timeline")->isChecked());

    window.commands()->trigger(QStringLiteral("window.resetLayout"));
    QVERIFY(browser->isVisible());
    QVERIFY(timeline->isVisible());
    QCOMPARE(window.dockWidgetArea(properties), Qt::RightDockWidgetArea);
    QCOMPARE(window.dockWidgetArea(browser), Qt::LeftDockWidgetArea);
    QVERIFY(action(window, "window.browser")->isChecked());
}

void InterfaceTest::focusModeGivesTheWindowToTheView() {
    MainWindow window;
    show(window);
    auto* toolbar = window.findChild<QToolBar*>(QStringLiteral("modelingToolbar"));
    const QSize normal = window.viewport()->size();
    window.commands()->trigger(QStringLiteral("window.timeline")); // the user's own arrangement

    window.commands()->trigger(QStringLiteral("window.focus"));
    QVERIFY(window.isViewportFocused());
    QVERIFY(action(window, "window.focus")->isChecked());
    for (const char* name : {"browserDock", "propertiesDock", "timelineDock"}) {
        QVERIFY(!window.findChild<QDockWidget*>(QString::fromLatin1(name))->isVisible());
    }
    QVERIFY(!toolbar->isVisible());
    QTRY_VERIFY(window.viewport()->width() > normal.width() + 300);
    // The menus stay, so there is always a way back.
    QVERIFY(window.mainMenu()->isVisible());

    window.commands()->trigger(QStringLiteral("window.focus"));
    QVERIFY(!window.isViewportFocused());
    QVERIFY(window.findChild<QDockWidget*>(QStringLiteral("browserDock"))->isVisible());
    QVERIFY(window.findChild<QDockWidget*>(QStringLiteral("propertiesDock"))->isVisible());
    // Back as the user had it, with the timeline still collapsed.
    QVERIFY(!window.findChild<QDockWidget*>(QStringLiteral("timelineDock"))->isVisible());
    QVERIFY(toolbar->isVisible());

    // Closing while focused remembers the layout with its panels.
    window.commands()->trigger(QStringLiteral("window.focus"));
    QVERIFY(window.close());
    MainWindow next;
    show(next);
    QVERIFY(next.findChild<QDockWidget*>(QStringLiteral("browserDock"))->isVisible());
    QVERIFY(!next.isViewportFocused());
}

void InterfaceTest::propertiesShowAndEditTheSelectedBody() {
    MainWindow window;
    show(window);
    lcad::app::PropertiesPanel* panel = window.properties();
    QCOMPARE(panel->heading(), QStringLiteral("Nothing selected"));
    QCOMPARE(panel->rows(), QStringList{QStringLiteral("Bodies: 0")});

    create(window, PrimitiveKind::Box,
           {{QStringLiteral("sizeX"), 100.0},
            {QStringLiteral("sizeY"), 60.0},
            {QStringLiteral("sizeZ"), 40.0}});
    const ObjectId box = window.document()->bodies()[0].id;
    QCOMPARE(panel->rows(), QStringList{QStringLiteral("Bodies: 1")});

    window.browser()->bodyItem(box)->setSelected(true);
    QCOMPARE(panel->heading(), QStringLiteral("Box"));
    QCOMPARE(panel->rows(), (QStringList{QStringLiteral("Volume: 240000 mm³"),
                                         QStringLiteral("Surface area: 24800 mm²"),
                                         QStringLiteral("Size: 100 × 60 × 40 mm"),
                                         QStringLiteral("Center of mass: 50, 30, 20 mm"),
                                         QStringLiteral("Faces, edges, vertices: 6, 12, 8")}));

    // The name and the visibility can be changed here, as edits to the document.
    auto* name = panel->findChild<QLineEdit*>(QStringLiteral("bodyName"));
    name->setText(QStringLiteral("Housing"));
    emit name->editingFinished();
    QCOMPARE(window.document()->findBody(box)->name, std::string("Housing"));
    // The panel catches up once the edit it made has gone through.
    QTRY_COMPARE(panel->heading(), QStringLiteral("Housing"));
    QCOMPARE(window.browser()->bodyItem(box)->text(0), QStringLiteral("Housing"));
    name->setText(QStringLiteral("  "));
    emit name->editingFinished();
    QCOMPARE(name->text(), QStringLiteral("Housing"));

    auto* visible = panel->findChild<QCheckBox*>(QStringLiteral("bodyVisible"));
    visible->click();
    QVERIFY(!window.document()->findBody(box)->visible);
    QCOMPARE(QString::fromStdString(window.document()->undoLabel()), QStringLiteral("Hide Body"));
    // Hidden, the body is still the one selected, so the panel still shows it.
    QCOMPARE(window.browser()->selectedBodies(), (std::vector<ObjectId>{box}));
    QTRY_VERIFY(!visible->isChecked());
    QCOMPARE(panel->heading(), QStringLiteral("Housing"));
    // And it follows changes made elsewhere.
    action(window, "edit.undo")->trigger();
    QVERIFY(visible->isChecked());
    action(window, "edit.undo")->trigger();
    QCOMPARE(name->text(), QStringLiteral("Box"));

    create(window, PrimitiveKind::Sphere);
    window.browser()->tree()->selectAll();
    QCOMPARE(panel->heading(), QStringLiteral("2 items selected"));
    window.browser()->tree()->clearSelection();
    QCOMPARE(panel->heading(), QStringLiteral("Nothing selected"));
}

void InterfaceTest::propertiesMeasureFacesEdgesAndVertices() {
    MainWindow window;
    show(window);
    create(window, PrimitiveKind::Box,
           {{QStringLiteral("sizeX"), 100.0},
            {QStringLiteral("sizeY"), 60.0},
            {QStringLiteral("sizeZ"), 40.0}});
    auto* viewport = window.viewport();
    viewport->setStandardView(StandardView::Top);
    viewport->grabFramebuffer();
    const auto click = [viewport](const QPointF& where) {
        QTest::mouseMove(viewport, where.toPoint());
        viewport->grabFramebuffer();
        QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, where.toPoint());
        viewport->grabFramebuffer();
    };
    lcad::app::PropertiesPanel* panel = window.properties();

    click(viewport->project(50.0, 30.0, 40.0));
    QCOMPARE(panel->heading(), QStringLiteral("Face of Box"));
    QCOMPARE(panel->rows(), QStringList{QStringLiteral("Area: 6000 mm²")});

    click(viewport->project(50.0, 0.0, 40.0));
    QCOMPARE(panel->heading(), QStringLiteral("Edge of Box"));
    QCOMPARE(panel->rows(), QStringList{QStringLiteral("Length: 100 mm")});

    click(viewport->project(100.0, 60.0, 40.0));
    QCOMPARE(panel->heading(), QStringLiteral("Vertex of Box"));
    QCOMPARE(panel->rows(), (QStringList{QStringLiteral("X: 100 mm"), QStringLiteral("Y: 60 mm"),
                                         QStringLiteral("Z: 40 mm")}));

    // While a command runs the panel belongs to the command, and comes back afterwards.
    window.startPrimitive(PrimitiveKind::Cone);
    QVERIFY(!panel->isVisible());
    window.cancelCommand();
    QVERIFY(panel->isVisible());
    QCOMPARE(panel->heading(), QStringLiteral("Vertex of Box"));
}

void InterfaceTest::browserFilterNarrowsTheList() {
    MainWindow window;
    create(window, PrimitiveKind::Box);
    create(window, PrimitiveKind::Sphere);
    create(window, PrimitiveKind::Box);
    const auto& bodies = window.document()->bodies();
    lcad::app::BrowserPanel* browser = window.browser();
    const auto shown = [&] {
        QStringList names;
        for (const auto& body : bodies) {
            if (!browser->bodyItem(body.id)->isHidden()) {
                names << QString::fromStdString(body.name);
            }
        }
        return names;
    };
    QCOMPARE(shown(), (QStringList{QStringLiteral("Box"), QStringLiteral("Sphere"),
                                   QStringLiteral("Box 2")}));

    QTest::keyClicks(browser->filterField(), QStringLiteral("bo"));
    QCOMPARE(shown(), (QStringList{QStringLiteral("Box"), QStringLiteral("Box 2")}));
    // A body added or renamed while filtering is filtered too.
    create(window, PrimitiveKind::Cone);
    QCOMPARE(shown(), (QStringList{QStringLiteral("Box"), QStringLiteral("Box 2")}));
    window.document()->renameBody(bodies[1].id, "Bobbin");
    QCOMPARE(shown(), (QStringList{QStringLiteral("Box"), QStringLiteral("Bobbin"),
                                   QStringLiteral("Box 2")}));

    browser->setFilter(QString());
    QCOMPARE(shown().size(), 4);
    // Filtering changes what is listed, never the document.
    QCOMPARE(bodies.size(), std::size_t{4});
}

void InterfaceTest::originGuidesBelongToTheirTab() {
    MainWindow window;
    show(window);
    create(window, PrimitiveKind::Sphere, {{QStringLiteral("positionX"), 200.0}});
    auto* viewport = window.viewport();
    lcad::app::BrowserPanel* browser = window.browser();
    QTreeWidgetItem* origin = browser->tree()->topLevelItem(0)->child(0);
    QCOMPARE(origin->text(0), QStringLiteral("Origin"));
    QCOMPARE(origin->childCount(), 6);
    QCOMPARE(browser->originItem(OriginItem::PlaneXY)->text(0), QStringLiteral("XY Plane"));
    QCOMPARE(browser->originItem(OriginItem::AxisZ)->text(0), QStringLiteral("Z Axis"));
    QVERIFY(!viewport->isOriginItemVisible(OriginItem::PlaneXY));

    // Ticking an entry shows that guide. It is not an edit: there is nothing to undo or save.
    browser->originItem(OriginItem::PlaneXY)->setCheckState(0, Qt::Checked);
    browser->originItem(OriginItem::AxisZ)->setCheckState(0, Qt::Checked);
    QVERIFY(viewport->isOriginItemVisible(OriginItem::PlaneXY));
    QVERIFY(viewport->isOriginItemVisible(OriginItem::AxisZ));
    QVERIFY(!viewport->isOriginItemVisible(OriginItem::PlaneYZ));
    QCOMPARE(QString::fromStdString(window.document()->undoLabel()),
             QStringLiteral("Create Sphere"));

    // Another document starts without them, and the first gets them back.
    window.newDocument();
    QVERIFY(!viewport->isOriginItemVisible(OriginItem::PlaneXY));
    QCOMPARE(browser->originItem(OriginItem::PlaneXY)->checkState(0), Qt::Unchecked);
    window.setCurrentIndex(0);
    QVERIFY(viewport->isOriginItemVisible(OriginItem::PlaneXY));
    QVERIFY(viewport->isOriginItemVisible(OriginItem::AxisZ));
    QCOMPARE(browser->originItem(OriginItem::AxisZ)->checkState(0), Qt::Checked);
    QCOMPARE(browser->originItem(OriginItem::AxisX)->checkState(0), Qt::Unchecked);

    browser->originItem(OriginItem::PlaneXY)->setCheckState(0, Qt::Unchecked);
    QVERIFY(!viewport->isOriginItemVisible(OriginItem::PlaneXY));
}

void InterfaceTest::hideShowAndIsolateAreUndoable() {
    MainWindow window;
    create(window, PrimitiveKind::Box);
    create(window, PrimitiveKind::Sphere);
    create(window, PrimitiveKind::Cone);
    const auto& bodies = window.document()->bodies();
    const auto visible = [&] {
        QString pattern;
        for (const auto& body : bodies) {
            pattern += body.visible ? QLatin1Char('1') : QLatin1Char('0');
        }
        return pattern;
    };
    CommandRegistry* commands = window.commands();
    // With nothing selected there is nothing to hide or isolate.
    QVERIFY(!commands->trigger(QStringLiteral("body.hide")));
    QVERIFY(!commands->trigger(QStringLiteral("body.isolate")));
    QVERIFY(!commands->trigger(QStringLiteral("body.showAll")));

    window.browser()->bodyItem(bodies[1].id)->setSelected(true);
    QVERIFY(commands->trigger(QStringLiteral("body.hide")));
    QCOMPARE(visible(), QStringLiteral("101"));
    QCOMPARE(QString::fromStdString(window.document()->undoLabel()), QStringLiteral("Hide"));
    // Hidden, it can be shown but not hidden again.
    QVERIFY(!action(window, "body.hide")->isEnabled());
    QVERIFY(commands->trigger(QStringLiteral("body.show")));
    QCOMPARE(visible(), QStringLiteral("111"));

    // Isolate hides the others, in one step.
    QVERIFY(commands->trigger(QStringLiteral("body.isolate")));
    QCOMPARE(visible(), QStringLiteral("010"));
    QCOMPARE(QString::fromStdString(window.document()->undoLabel()), QStringLiteral("Isolate"));
    action(window, "edit.undo")->trigger();
    QCOMPARE(visible(), QStringLiteral("111"));
    action(window, "edit.redo")->trigger();
    QCOMPARE(visible(), QStringLiteral("010"));

    // Show All brings back every hidden body, in one step.
    QVERIFY(commands->trigger(QStringLiteral("body.showAll")));
    QCOMPARE(visible(), QStringLiteral("111"));
    action(window, "edit.undo")->trigger();
    QCOMPARE(visible(), QStringLiteral("010"));
}

void InterfaceTest::viewportMenuOffersWhatApplies() {
    MainWindow window;
    show(window);
    create(window, PrimitiveKind::Box,
           {{QStringLiteral("sizeX"), 100.0},
            {QStringLiteral("sizeY"), 60.0},
            {QStringLiteral("sizeZ"), 40.0}});
    create(window, PrimitiveKind::Sphere, {{QStringLiteral("positionX"), 250.0}});
    const ObjectId box = window.document()->bodies()[0].id;
    auto* viewport = window.viewport();
    viewport->setStandardView(StandardView::Top);
    viewport->grabFramebuffer();
    const QPoint onTheBox = viewport->project(50.0, 30.0, 40.0).toPoint();
    const QPoint onNothing = viewport->project(150.0, 30.0, 0.0).toPoint();

    // Over empty space: only what always applies.
    std::unique_ptr<QMenu> menu(window.createViewportMenu(onNothing));
    QCOMPARE(entriesOf(menu.get()), (QStringList{QStringLiteral("Fit"), QStringLiteral("Home")}));

    // Over a face: the face becomes the selection, and the menu is about it and its body.
    menu.reset(window.createViewportMenu(onTheBox));
    QCOMPARE(viewport->selection().size(), std::size_t{1});
    QCOMPARE(viewport->selection()[0].kind, SelectedItem::Kind::Face);
    QCOMPARE(entriesOf(menu.get()),
             (QStringList{QStringLiteral("Select Other"), QStringLiteral("Zoom to Selection"),
                          QStringLiteral("Hide"), QStringLiteral("Isolate"), QStringLiteral("Fit"),
                          QStringLiteral("Home")}));
    // Delete is for whole bodies, and a face is selected, so it is not offered.
    QVERIFY(!entriesOf(menu.get()).contains(QStringLiteral("Delete")));

    // Choosing Hide from it hides the face's body.
    for (QAction* entry : menu->actions()) {
        if (entry->text().remove(QLatin1Char('&')) == QStringLiteral("Hide")) {
            entry->trigger();
        }
    }
    QVERIFY(!window.document()->findBody(box)->visible);

    // Now something is hidden, so Show All is offered, even over empty space.
    menu.reset(window.createViewportMenu(onNothing));
    QCOMPARE(entriesOf(menu.get()), (QStringList{QStringLiteral("Show All Bodies"),
                                                 QStringLiteral("Fit"), QStringLiteral("Home")}));

    // The real thing: a right-click puts the menu up at the pointer.
    window.commands()->trigger(QStringLiteral("body.showAll"));
    window.commands()->trigger(QStringLiteral("select.bodies"));
    viewport->grabFramebuffer();
    QStringList offered;
    chooseFromNextMenu(QStringLiteral("Delete"), &offered);
    emit viewport->customContextMenuRequested(onTheBox);
    QTRY_VERIFY(!offered.isEmpty());
    QVERIFY(offered.contains(QStringLiteral("Delete")));
    QTRY_COMPARE(window.document()->bodies().size(), std::size_t{1});
    QCOMPARE(window.document()->bodies()[0].name, std::string("Sphere"));
}

void InterfaceTest::viewportMenuReachesWhatLiesBehind() {
    MainWindow window;
    show(window);
    create(window, PrimitiveKind::Box,
           {{QStringLiteral("sizeX"), 100.0},
            {QStringLiteral("sizeY"), 60.0},
            {QStringLiteral("sizeZ"), 40.0}});
    const lcad::kernel::Shape shape = window.document()->bodies()[0].shape;
    auto* viewport = window.viewport();
    viewport->setStandardView(StandardView::Top);
    window.commands()->trigger(QStringLiteral("select.faces"));
    viewport->grabFramebuffer();

    const std::unique_ptr<QMenu> menu(
        window.createViewportMenu(viewport->project(50.0, 30.0, 40.0).toPoint()));
    const QMenu* other = menu->findChild<QMenu*>(QStringLiteral("menu.selectOther"));
    QVERIFY(other != nullptr);
    // The top face, which a click selects, and the bottom face under it. Both are 100 by 60.
    QCOMPARE(entriesOf(other), (QStringList{QStringLiteral("Face of Box · area 6000 mm²"),
                                            QStringLiteral("Face of Box · area 6000 mm²")}));

    other->actions().at(1)->trigger();
    const std::vector<SelectedItem> selection = viewport->selection();
    QCOMPARE(selection.size(), std::size_t{1});
    const auto face = lcad::kernel::boundingBox(
        lcad::kernel::subShape(shape, lcad::kernel::SubShapeKind::Face, selection[0].index));
    // The bottom one: it lies at Z = 0.
    QVERIFY(qAbs(face->max.z) < 1e-6);
}

void InterfaceTest::gridAndDisplayStyleAreCommandsAndRemembered() {
    {
        MainWindow window;
        show(window);
        auto* viewport = window.viewport();
        // A new installation shows the grid, and bodies shaded with edges.
        QVERIFY(viewport->isGridVisible());
        QVERIFY(action(window, "view.grid")->isChecked());
        QCOMPARE(viewport->displayStyle(), DisplayStyle::ShadedWithEdges);
        QVERIFY(action(window, "view.style.shadedEdges")->isChecked());
        create(window, PrimitiveKind::Box);
        const QImage gridded = viewport->grabFramebuffer();
        keepFrame(window.grab().toImage(), "interface-grid");

        window.commands()->trigger(QStringLiteral("view.grid"));
        QVERIFY(!viewport->isGridVisible());
        QVERIFY(differingFraction(gridded, viewport->grabFramebuffer()) > 0.005);

        window.commands()->trigger(QStringLiteral("view.style.wireframe"));
        QCOMPARE(viewport->displayStyle(), DisplayStyle::Wireframe);
        QVERIFY(action(window, "view.style.wireframe")->isChecked());
        QVERIFY(!action(window, "view.style.shadedEdges")->isChecked());
        // The toolbar's style button offers the same five commands.
        const auto* button = window.findChild<QToolButton*>(QStringLiteral("displayStyleButton"));
        QCOMPARE(button->menu()->actions().size(), 5);
        QVERIFY(button->menu()->actions().contains(action(window, "view.style.hiddenLine")));
        QVERIFY(!button->icon().isNull());

        window.commands()->trigger(QStringLiteral("view.axes"));
        window.commands()->trigger(QStringLiteral("view.animate"));
        window.commands()->trigger(QStringLiteral("view.navigation.middleOrbits"));
        QVERIFY(!viewport->areAxesVisible());
        QVERIFY(!viewport->isAnimated());
    }
    // As after a restart.
    const MainWindow window;
    QVERIFY(!window.viewport()->isGridVisible());
    QVERIFY(!window.viewport()->areAxesVisible());
    QVERIFY(!window.viewport()->isAnimated());
    QCOMPARE(window.viewport()->displayStyle(), DisplayStyle::Wireframe);
    QCOMPARE(window.viewport()->navigationPreset(), lcad::viewport::NavigationPreset::MiddleOrbits);
    QVERIFY(action(window, "view.navigation.middleOrbits")->isChecked());
    QVERIFY(!action(window, "view.grid")->isChecked());
}

QTEST_MAIN(InterfaceTest)
#include "InterfaceTest.moc"
