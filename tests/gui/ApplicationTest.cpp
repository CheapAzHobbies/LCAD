// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/Application.h>
#include <lcad/app/BrowserPanel.h>
#include <lcad/app/Commands.h>
#include <lcad/app/MainWindow.h>
#include <lcad/app/PrimitivePanel.h>
#include <lcad/app/Theme.h>
#include <lcad/core/Log.h>
#include <lcad/document/File.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/Version.h>
#include <lcad/viewport/ViewportWidget.h>

#include <FrameChecks.h>
#include <GraphicsMessages.h>
#include <GuiHelpers.h>

#include <QAction>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QGuiApplication>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QSettings>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>

#include <xcb/xcb.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>

using lcad::app::BrowserPanel;
using lcad::app::MainWindow;
using lcad::app::PrimitiveKind;
using lcad::document::ObjectId;
using lcad::test::answerNextMessageBox;
using lcad::test::chooseFromNextMenu;
using lcad::test::differingFraction;
using lcad::test::drawnFraction;
using lcad::test::keepFrame;
using lcad::viewport::SelectedItem;
using lcad::viewport::StandardView;

namespace {

const double pi = 3.14159265358979323846;

// What the X server shows for `widget`, as opposed to what the widget believes it drew.
QImage onScreen(const QWidget* widget) {
    const QWidget* window = widget->window();
    const qreal scale = window->devicePixelRatioF();
    const QImage whole = window->screen()->grabWindow(window->winId()).toImage();
    const QPoint topLeft = widget->mapTo(window, QPoint(0, 0)) * scale;
    return whole.copy(QRect(topLeft, widget->size() * scale));
}

// XWayland does not let X11 clients read window contents back, so the screen cannot be checked
// there. It announces itself with an X extension of its own.
bool isXWayland() {
    const auto* x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    if (x11 == nullptr) {
        return false;
    }
    const char* name = "XWAYLAND";
    xcb_connection_t* connection = x11->connection();
    xcb_query_extension_reply_t* reply = xcb_query_extension_reply(
        connection, xcb_query_extension(connection, static_cast<uint16_t>(std::strlen(name)), name),
        nullptr);
    const bool present = reply != nullptr && reply->present != 0;
    std::free(reply);
    return present;
}

QAction* action(const MainWindow& window, const char* name) {
    return window.findChild<QAction*>(QString::fromLatin1(name));
}

double volumeOf(const MainWindow& window, std::size_t index) {
    return lcad::kernel::volume(window.document()->bodies().at(index).shape);
}

bool near(double actual, double expected) {
    return std::abs(actual - expected) <= 1e-9 * std::abs(expected);
}

} // namespace

class ApplicationTest : public QObject {
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

    void versionStringIsThreeNumbers();
    void windowHasItsFiveRegions();
    void aboutTextNamesTheLibrariesInUse();
    void viewportAndMenusReachTheScreenTogether();

    void createSaveCloseReopenKeepsTheGeometry();
    void everyPrimitiveCanBeCreated();
    void panelPreviewsAndRefusesSizesThatMakeNoSolid();
    void cancelingACommandLeavesTheDocumentAlone();
    void sizesCanBeTypedAndConfirmedFromTheKeyboard();
    void undoAndRedoWorkFromTheMenu();

    void browserListsBodiesAndEditsThem();
    void browserContextMenuActsOnTheBodyClicked();
    void selectionIsSharedBetweenBrowserAndViewport();
    void statusBarMeasuresWhatIsPicked();
    void deleteRemovesTheSelectedBodiesInOneStep();
    void viewMenuDrivesTheCamera();
    void zoomDirectionIsRemembered();

    void closingWithUnsavedChangesAsksFirst();
    void aFileThatCannotBeOpenedLeavesTheDocumentInPlace();
    void savingToAnImpossiblePlaceReportsIt();
    void logGoesToAFile();

private:
    // Runs the create-primitive command with the given values and accepts it.
    static void create(MainWindow& window, PrimitiveKind kind,
                       const QList<std::pair<QString, double>>& values = {});

    QTemporaryDir m_directory;
    lcad::test::GraphicsMessages m_graphicsMessages;
};

void ApplicationTest::init() {
    // Qt reports trouble with OpenGL contexts and surfaces as warnings and carries on.
    QTest::failOnWarning(QRegularExpression(QStringLiteral("OpenGL|QRhi|context|surface"),
                                            QRegularExpression::CaseInsensitiveOption));
    QSettings().clear();
    // Frame comparisons in these tests are about the model, so the reference grid stays off
    // here. InterfaceTest covers the grid.
    QSettings().setValue(QStringLiteral("view/grid"), false);
    lcad::app::Theme::instance().load();
    QVERIFY(m_directory.isValid());
}

void ApplicationTest::cleanup() {
    const QStringList complaints = m_graphicsMessages.take();
    QVERIFY2(complaints.isEmpty(), qPrintable(complaints.join(QStringLiteral(" | "))));
}

void ApplicationTest::create(MainWindow& window, PrimitiveKind kind,
                             const QList<std::pair<QString, double>>& values) {
    window.startPrimitive(kind);
    for (const auto& [field, value] : values) {
        window.activePanel()->setValue(field, value);
    }
    window.acceptCommand();
}

void ApplicationTest::versionStringIsThreeNumbers() {
    const QRegularExpression pattern(QStringLiteral(R"(^\d+\.\d+\.\d+$)"));
    QVERIFY2(pattern.match(lcad::app::versionString()).hasMatch(),
             qPrintable(lcad::app::versionString()));
}

void ApplicationTest::windowHasItsFiveRegions() {
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    QCOMPARE(window.windowTitle(), QStringLiteral("Untitled[*] - LCAD"));

    // Top bar: the menus, the quick buttons, the document tabs, and the command search.
    auto* topBar = window.findChild<QWidget*>(QStringLiteral("topBar"));
    QVERIFY(topBar != nullptr);
    QCOMPARE(window.menuWidget(), topBar);
    QStringList menus;
    for (const QAction* menu : window.mainMenu()->actions()) {
        menus << menu->text().remove(QLatin1Char('&'));
    }
    QCOMPARE(
        menus,
        (QStringList{QStringLiteral("File"), QStringLiteral("Edit"), QStringLiteral("Create"),
                     QStringLiteral("Modify"), QStringLiteral("Assemble"), QStringLiteral("Sketch"),
                     QStringLiteral("View"), QStringLiteral("Window"), QStringLiteral("Help")}));
    QList<QAction*> quickActions;
    for (const QToolButton* button :
         topBar->findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly)) {
        quickActions << button->defaultAction();
    }
    QCOMPARE(quickActions,
             (QList<QAction*>{action(window, "file.new"), action(window, "file.open"),
                              action(window, "file.save"), action(window, "edit.undo"),
                              action(window, "edit.redo"), action(window, "window.search")}));
    auto* tabs = topBar->findChild<QTabBar*>(QStringLiteral("documentTabs"));
    QVERIFY(tabs != nullptr);
    QCOMPARE(tabs->count(), 1);
    QCOMPARE(tabs->tabText(0), QStringLiteral("Untitled"));

    // Toolbar: the Create group with one tool for each primitive.
    auto* toolbar = window.findChild<QToolBar*>(QStringLiteral("modelingToolbar"));
    QVERIFY(toolbar != nullptr);
    QVERIFY(toolbar->isVisible());
    for (const char* name :
         {"create.box", "create.cylinder", "create.sphere", "create.cone", "create.torus"}) {
        QVERIFY2(toolbar->actions().contains(action(window, name)), name);
    }
    QVERIFY(topBar->geometry().bottom() <= toolbar->geometry().top());

    // Browser on the left, viewport in the middle, timeline underneath.
    auto* browserDock = window.findChild<QDockWidget*>(QStringLiteral("browserDock"));
    auto* timelineDock = window.findChild<QDockWidget*>(QStringLiteral("timelineDock"));
    QVERIFY(browserDock != nullptr && browserDock->isVisible());
    QVERIFY(timelineDock != nullptr && timelineDock->isVisible());
    QCOMPARE(window.dockWidgetArea(browserDock), Qt::LeftDockWidgetArea);
    QCOMPARE(window.dockWidgetArea(timelineDock), Qt::BottomDockWidgetArea);
    QCOMPARE(window.centralWidget(), window.viewport());
    const QRect view = window.viewport()->geometry();
    QVERIFY(browserDock->geometry().right() <= view.left());
    QVERIFY(timelineDock->geometry().top() >= view.bottom());
    QVERIFY(toolbar->geometry().bottom() <= view.top());
    // The viewport is the main thing in the window.
    QVERIFY(view.width() * view.height() > window.width() * window.height() / 2);

    // The properties panel is on the right, saying that nothing is selected.
    auto* propertiesDock = window.findChild<QDockWidget*>(QStringLiteral("propertiesDock"));
    QVERIFY(propertiesDock != nullptr && propertiesDock->isVisible());
    QCOMPARE(window.dockWidgetArea(propertiesDock), Qt::RightDockWidgetArea);
    QVERIFY(propertiesDock->geometry().left() >= view.right());
    QCOMPARE(propertiesDock->windowTitle(), QStringLiteral("Properties"));
    keepFrame(window.grab().toImage(), "main-window-empty");
}

void ApplicationTest::aboutTextNamesTheLibrariesInUse() {
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    // Rendering a frame makes sure the viewport has an OpenGL context to describe.
    window.viewport()->grabFramebuffer();

    const QString about = window.aboutText();

    QVERIFY(about.contains(lcad::app::versionString()));
    QVERIFY(about.contains(QStringLiteral("GNU General Public License, version 3 or later")));
    QVERIFY(about.contains(QString::fromLatin1(qVersion())));
    QVERIFY(about.contains(QString::fromStdString(lcad::kernel::occtVersion())));
    QVERIFY(!window.viewport()->graphicsInfo().isEmpty());
    QVERIFY(about.contains(window.viewport()->graphicsInfo()));
}

// The viewport shares an OpenGL context with the code Qt uses to put every widget on screen.
// Getting that wrong leaves the widget's own framebuffer looking right while the window shows
// nothing, or shows menus with corrupted text. So this test looks at the screen.
void ApplicationTest::viewportAndMenusReachTheScreenTogether() {
    MainWindow window;
    window.resize(1000, 700);
    window.document()->addBody("Box", lcad::kernel::makeBox(100.0, 60.0, 40.0));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    if (isXWayland()) {
        QSKIP("XWayland does not let X11 clients read the screen; run this test with ctest.");
    }

    const QColor background = window.viewport()->backgroundColor();
    QTRY_VERIFY_WITH_TIMEOUT(drawnFraction(onScreen(window.viewport()), background) > 0.10, 5000);
    const QImage shownViewport = onScreen(window.viewport());
    QWidget* topBar = window.findChild<QWidget*>(QStringLiteral("topBar"));
    const QImage shownTopBar = onScreen(topBar);
    keepFrame(window.screen()->grabWindow(window.winId()).toImage(), "main-window-on-screen");

    QVERIFY(drawnFraction(shownViewport, background) < 0.95);
    // The top bar on screen, with its menus and buttons, is what Qt's own painter produces.
    const double topBarDifference = differingFraction(shownTopBar, topBar->grab().toImage());
    QVERIFY2(
        topBarDifference < 0.01,
        qPrintable(QStringLiteral("top bar differs in %1 of its pixels").arg(topBarDifference)));
    // And so is the browser, which sits beside the viewport.
    const BrowserPanel* browser = window.browser();
    QVERIFY(differingFraction(onScreen(browser),
                              const_cast<BrowserPanel*>(browser)->grab().toImage()) < 0.01);
}

// The acceptance test of Phase 1, through the interface: create a document, add a solid, save,
// close, reopen, and verify the geometry is identical.
void ApplicationTest::createSaveCloseReopenKeepsTheGeometry() {
    const QString path = m_directory.filePath(QStringLiteral("bracket blank.lcad"));
    lcad::kernel::Shape saved;
    {
        MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.document()->bodies().empty());
        QVERIFY(!window.isWindowModified());

        action(window, "create.box")->trigger();
        QVERIFY(window.activePanel() != nullptr);
        window.activePanel()->setValue(QStringLiteral("sizeX"), 120.0);
        window.activePanel()->setValue(QStringLiteral("sizeY"), 80.0);
        window.activePanel()->setValue(QStringLiteral("sizeZ"), 12.5);
        window.activePanel()->setValue(QStringLiteral("positionX"), -60.0);
        window.activePanel()->setValue(QStringLiteral("positionZ"), 5.0);
        window.activePanel()->findChild<QPushButton*>()->click(); // OK
        create(window, PrimitiveKind::Cylinder,
               {{QStringLiteral("diameter"), 30.0}, {QStringLiteral("height"), 40.0}});

        QCOMPARE(window.document()->bodies().size(), std::size_t{2});
        QVERIFY(window.isWindowModified());
        QVERIFY(window.filePath().isEmpty());

        QString error;
        QVERIFY2(window.saveTo(path, &error), qPrintable(error));
        QVERIFY(!window.isWindowModified());
        QCOMPARE(window.documentName(), QStringLiteral("bracket blank"));
        QCOMPARE(window.windowTitle(), QStringLiteral("bracket blank[*] - LCAD"));
        saved = window.document()->bodies()[0].shape;
        QVERIFY(window.close());
    }

    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QString error;
    QVERIFY2(window.loadFrom(path, &error), qPrintable(error));

    QVERIFY(window.loadWarnings().isEmpty());
    QVERIFY(!window.isWindowModified());
    QCOMPARE(window.filePath(), path);
    const auto& bodies = window.document()->bodies();
    QCOMPARE(bodies.size(), std::size_t{2});
    QCOMPARE(bodies[0].name, std::string("Box"));
    QCOMPARE(bodies[1].name, std::string("Cylinder"));

    // Measured on the exact geometry, against the closed-form values for what was entered.
    using namespace lcad::kernel;
    const Shape& box = bodies[0].shape;
    QVERIFY(isValid(box));
    QVERIFY(near(volume(box), 120.0 * 80.0 * 12.5));
    QVERIFY(near(surfaceArea(box), 2.0 * (120.0 * 80.0 + 80.0 * 12.5 + 120.0 * 12.5)));
    const TopologyCount count = countTopology(box);
    QCOMPARE(count.solids, 1);
    QCOMPARE(count.faces, 6);
    QCOMPARE(count.edges, 12);
    QCOMPARE(count.vertices, 8);
    const auto bounds = boundingBox(box);
    QVERIFY(bounds.has_value());
    QVERIFY(std::abs(bounds->min.x + 60.0) < 1e-6 && std::abs(bounds->max.x - 60.0) < 1e-6);
    QVERIFY(std::abs(bounds->min.y) < 1e-6 && std::abs(bounds->max.y - 80.0) < 1e-6);
    QVERIFY(std::abs(bounds->min.z - 5.0) < 1e-6 && std::abs(bounds->max.z - 17.5) < 1e-6);
    // And against the shape as it was before saving.
    QVERIFY(near(volume(box), volume(saved)));
    QVERIFY(near(surfaceArea(box), surfaceArea(saved)));

    const Shape& cylinder = bodies[1].shape;
    QVERIFY(isValid(cylinder));
    QVERIFY(near(volume(cylinder), pi * 15.0 * 15.0 * 40.0));

    // The reopened document is on screen, in the browser and in the viewport.
    QVERIFY(window.browser()->bodyItem(bodies[0].id) != nullptr);
    QCOMPARE(window.browser()->tree()->topLevelItem(0)->text(0), QStringLiteral("bracket blank"));
    QVERIFY(drawnFraction(window.viewport()->grabFramebuffer(),
                          window.viewport()->backgroundColor()) > 0.10);
    keepFrame(window.grab().toImage(), "main-window-reopened");

    // The file carries a preview image of the model.
    QImage preview;
    QVERIFY(preview.loadFromData(QByteArray::fromStdString(
        lcad::document::readThumbnail(std::filesystem::path(path.toStdString())))));
    QVERIFY(preview.width() <= 256 && preview.height() <= 256 && preview.width() > 32);
    QVERIFY(drawnFraction(preview, window.viewport()->backgroundColor()) > 0.05);
}

void ApplicationTest::everyPrimitiveCanBeCreated() {
    MainWindow window;

    create(window, PrimitiveKind::Box);
    create(window, PrimitiveKind::Cylinder);
    create(window, PrimitiveKind::Sphere);
    create(window, PrimitiveKind::Cone);
    create(window, PrimitiveKind::Torus);
    create(window, PrimitiveKind::Box);

    const auto& bodies = window.document()->bodies();
    QCOMPARE(bodies.size(), std::size_t{6});
    QStringList names;
    for (const auto& body : bodies) {
        names << QString::fromStdString(body.name);
        QVERIFY(lcad::kernel::isValid(body.shape));
    }
    QCOMPARE(names, (QStringList{QStringLiteral("Box"), QStringLiteral("Cylinder"),
                                 QStringLiteral("Sphere"), QStringLiteral("Cone"),
                                 QStringLiteral("Torus"), QStringLiteral("Box 2")}));
    // The sizes the panel starts with: 50 mm throughout, a pointed cone, a 100 by 20 ring.
    QVERIFY(near(volumeOf(window, 0), 125000.0));
    QVERIFY(near(volumeOf(window, 1), pi * 25.0 * 25.0 * 50.0));
    QVERIFY(near(volumeOf(window, 2), 4.0 / 3.0 * pi * 25.0 * 25.0 * 25.0));
    QVERIFY(near(volumeOf(window, 3), pi * 25.0 * 25.0 * 50.0 / 3.0));
    QVERIFY(near(volumeOf(window, 4), 2.0 * pi * pi * 50.0 * 10.0 * 10.0));
    QCOMPARE(QString::fromStdString(window.document()->undoLabel()), QStringLiteral("Create Box"));
}

void ApplicationTest::panelPreviewsAndRefusesSizesThatMakeNoSolid() {
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const QColor background = window.viewport()->backgroundColor();
    const double empty = drawnFraction(window.viewport()->grabFramebuffer(), background);

    action(window, "create.torus")->trigger();
    auto* panel = window.activePanel();
    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("propertiesDock"));
    QVERIFY(panel != nullptr);
    QVERIFY(dock->isVisible());
    QCOMPARE(dock->windowTitle(), QStringLiteral("Create Torus"));
    QCOMPARE(window.dockWidgetArea(dock), Qt::RightDockWidgetArea);
    // The preview is in the viewport and not yet in the document.
    QVERIFY(drawnFraction(window.viewport()->grabFramebuffer(), background) > empty + 0.05);
    QVERIFY(window.document()->bodies().empty());
    QVERIFY(!window.isWindowModified());
    keepFrame(window.grab().toImage(), "main-window-create-torus");

    // A tube wider than the ring is not a torus.
    panel->setValue(QStringLiteral("tubeDiameter"), 150.0);
    QVERIFY(panel->shape().isNull());
    QCOMPARE(panel->errorText(),
             QStringLiteral("The tube diameter must be smaller than the ring diameter."));
    QVERIFY(panel->findChild<QLabel*>(QStringLiteral("errorText"))->isVisible());
    auto* ok = panel->findChild<QPushButton*>();
    QVERIFY(!ok->isEnabled());
    QVERIFY(drawnFraction(window.viewport()->grabFramebuffer(), background) < empty + 0.005);
    window.acceptCommand();
    QVERIFY(window.document()->bodies().empty());
    QCOMPARE(window.activePanel(), panel);

    panel->setValue(QStringLiteral("tubeDiameter"), 30.0);
    QVERIFY(panel->errorText().isEmpty());
    QVERIFY(ok->isEnabled());
    ok->click();

    QCOMPARE(window.document()->bodies().size(), std::size_t{1});
    QVERIFY(near(volumeOf(window, 0), 2.0 * pi * pi * 50.0 * 15.0 * 15.0));
    QVERIFY(window.activePanel() == nullptr);
    // The panel hands the place back to the properties.
    QCOMPARE(dock->windowTitle(), QStringLiteral("Properties"));
}

void ApplicationTest::cancelingACommandLeavesTheDocumentAlone() {
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const QImage before = window.viewport()->grabFramebuffer();
    QVERIFY(!action(window, "edit.cancel")->isEnabled());

    action(window, "create.sphere")->trigger();
    QVERIFY(action(window, "edit.cancel")->isEnabled());
    QVERIFY(differingFraction(before, window.viewport()->grabFramebuffer()) > 0.05);
    action(window, "edit.cancel")->trigger(); // what Esc does

    QVERIFY(window.activePanel() == nullptr);
    QVERIFY(window.document()->bodies().empty());
    QVERIFY(!window.document()->canUndo());
    QVERIFY(!window.isWindowModified());
    // Once the panel has gone and the viewport has its full width back, it looks as it did.
    QTRY_VERIFY(differingFraction(before, window.viewport()->grabFramebuffer()) < 0.001);

    // Starting another command replaces the one in progress.
    action(window, "create.box")->trigger();
    const auto* first = window.activePanel();
    action(window, "create.cone")->trigger();
    QVERIFY(window.activePanel() != first);
    QCOMPARE(window.activePanel()->kind(), PrimitiveKind::Cone);
}

void ApplicationTest::sizesCanBeTypedAndConfirmedFromTheKeyboard() {
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const auto typeInto = [&window](const char* field, const char* text) {
        auto* box = window.activePanel()->findChild<QDoubleSpinBox*>(QString::fromLatin1(field));
        box->setFocus();
        box->selectAll();
        QTest::keyClicks(box, QString::fromLatin1(text));
        return box;
    };

    action(window, "create.cylinder")->trigger();
    typeInto("diameter", "30");
    // The preview follows the typing; nothing has to be confirmed box by box.
    QCOMPARE(window.activePanel()->value(QStringLiteral("diameter")), 30.0);
    QDoubleSpinBox* height = typeInto("height", "12.5");
    // Enter takes in what was typed and accepts the command.
    QTest::keyClick(height, Qt::Key_Return);

    QCOMPARE(window.document()->bodies().size(), std::size_t{1});
    QVERIFY(near(volumeOf(window, 0), pi * 15.0 * 15.0 * 12.5));
    QVERIFY(window.activePanel() == nullptr);

    // Enter does nothing while the sizes describe no solid, and Esc leaves the command.
    action(window, "create.sphere")->trigger();
    QDoubleSpinBox* diameter = typeInto("diameter", "0");
    QTest::keyClick(diameter, Qt::Key_Return);
    QCOMPARE(window.document()->bodies().size(), std::size_t{1});
    QVERIFY(window.activePanel() != nullptr);
    QVERIFY(!window.activePanel()->errorText().isEmpty());
    QTest::keyClick(diameter, Qt::Key_Escape);
    QVERIFY(window.activePanel() == nullptr);
    QCOMPARE(window.document()->bodies().size(), std::size_t{1});
}

void ApplicationTest::undoAndRedoWorkFromTheMenu() {
    MainWindow window;
    QAction* undo = action(window, "edit.undo");
    QAction* redo = action(window, "edit.redo");
    QVERIFY(!undo->isEnabled());
    QVERIFY(!redo->isEnabled());

    create(window, PrimitiveKind::Box);
    create(window, PrimitiveKind::Sphere);
    QVERIFY(undo->isEnabled());
    QCOMPARE(undo->text().remove(QLatin1Char('&')), QStringLiteral("Undo Create Sphere"));

    undo->trigger();
    QCOMPARE(window.document()->bodies().size(), std::size_t{1});
    QCOMPARE(undo->text().remove(QLatin1Char('&')), QStringLiteral("Undo Create Box"));
    QCOMPARE(redo->text().remove(QLatin1Char('&')), QStringLiteral("Redo Create Sphere"));
    QVERIFY(window.browser()
                ->tree()
                ->findItems(QStringLiteral("Sphere"), Qt::MatchRecursive)
                .isEmpty());

    undo->trigger();
    QVERIFY(window.document()->bodies().empty());
    QVERIFY(!undo->isEnabled());
    QVERIFY(!window.isWindowModified());

    redo->trigger();
    redo->trigger();
    QCOMPARE(window.document()->bodies().size(), std::size_t{2});
    QVERIFY(!redo->isEnabled());
    QVERIFY(window.isWindowModified());
}

void ApplicationTest::browserListsBodiesAndEditsThem() {
    MainWindow window;
    create(window, PrimitiveKind::Box);
    create(window, PrimitiveKind::Sphere);
    const ObjectId box = window.document()->bodies()[0].id;
    const ObjectId sphere = window.document()->bodies()[1].id;
    QTreeWidget* tree = window.browser()->tree();

    // Document, then Bodies, then one entry per body in order.
    QCOMPARE(tree->topLevelItemCount(), 1);
    QTreeWidgetItem* root = tree->topLevelItem(0);
    QCOMPARE(root->text(0), QStringLiteral("Untitled"));
    QCOMPARE(root->childCount(), 5);
    QCOMPARE(root->child(0)->text(0), QStringLiteral("Origin"));
    // Joints come last, hidden while there are none; components would come before them.
    QCOMPARE(root->child(4)->text(0), QStringLiteral("Joints"));
    QVERIFY(root->child(4)->isHidden());
    // Construction planes have a group of their own, hidden while there are none.
    QCOMPARE(root->child(3)->text(0), QStringLiteral("Construction"));
    QVERIFY(root->child(3)->isHidden());
    // The entry for sketches is not shown while the document has none.
    QCOMPARE(root->child(2)->text(0), QStringLiteral("Sketches"));
    QVERIFY(root->child(2)->isHidden());
    QTreeWidgetItem* group = root->child(1);
    QCOMPARE(group->text(0), QStringLiteral("Bodies"));
    QCOMPARE(group->childCount(), 2);
    QCOMPARE(group->child(0), window.browser()->bodyItem(box));
    QCOMPARE(group->child(0)->text(0), QStringLiteral("Box"));
    QCOMPARE(group->child(1)->text(0), QStringLiteral("Sphere"));
    QCOMPARE(group->child(0)->checkState(0), Qt::Checked);

    // Renaming in the tree renames in the document, as an undoable edit.
    group->child(0)->setText(0, QStringLiteral("Housing"));
    QCOMPARE(window.document()->findBody(box)->name, std::string("Housing"));
    QCOMPARE(QString::fromStdString(window.document()->undoLabel()), QStringLiteral("Rename Body"));
    // An empty name is refused and the entry goes back to what the document says.
    group->child(0)->setText(0, QStringLiteral("  "));
    QCOMPARE(group->child(0)->text(0), QStringLiteral("Housing"));
    QCOMPARE(window.document()->findBody(box)->name, std::string("Housing"));

    // The check box shows and hides.
    group->child(1)->setCheckState(0, Qt::Unchecked);
    QVERIFY(!window.document()->findBody(sphere)->visible);
    QCOMPARE(QString::fromStdString(window.document()->undoLabel()), QStringLiteral("Hide Body"));

    // And the tree follows changes made elsewhere, such as undo.
    action(window, "edit.undo")->trigger();
    QCOMPARE(group->child(1)->checkState(0), Qt::Checked);
    action(window, "edit.undo")->trigger();
    QCOMPARE(group->child(0)->text(0), QStringLiteral("Box"));
    window.document()->removeBody(box);
    QCOMPARE(group->childCount(), 1);
    QCOMPARE(group->child(0)->text(0), QStringLiteral("Sphere"));
    action(window, "edit.undo")->trigger();
    // Back in first place, not appended.
    QCOMPARE(group->child(0)->text(0), QStringLiteral("Box"));
    QCOMPARE(group->child(1)->text(0), QStringLiteral("Sphere"));
}

void ApplicationTest::browserContextMenuActsOnTheBodyClicked() {
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    create(window, PrimitiveKind::Box);
    create(window, PrimitiveKind::Sphere);
    const ObjectId box = window.document()->bodies()[0].id;
    const ObjectId sphere = window.document()->bodies()[1].id;
    QTreeWidget* tree = window.browser()->tree();
    const auto rightClick = [&window, tree](ObjectId body) {
        const QPoint where = tree->visualItemRect(window.browser()->bodyItem(body)).center();
        emit tree->customContextMenuRequested(where);
    };

    QStringList offered;
    chooseFromNextMenu(QStringLiteral("Hide"), &offered);
    rightClick(sphere);
    QTRY_VERIFY(!offered.isEmpty());
    QCOMPARE(offered, (QStringList{QStringLiteral("Rename"), QStringLiteral("Hide"),
                                   QStringLiteral("Isolate"), QStringLiteral("Delete")}));
    QVERIFY(!window.document()->findBody(sphere)->visible);

    // A hidden body is offered Show instead.
    offered.clear();
    chooseFromNextMenu(QStringLiteral("Show"), &offered);
    rightClick(sphere);
    QTRY_VERIFY(!offered.isEmpty());
    QVERIFY(offered.contains(QStringLiteral("Show")) && !offered.contains(QStringLiteral("Hide")));
    QVERIFY(window.document()->findBody(sphere)->visible);

    // Delete acts on the body that was clicked, whatever else was selected before.
    window.browser()->bodyItem(sphere)->setSelected(true);
    chooseFromNextMenu(QStringLiteral("Delete"));
    rightClick(box);
    QTRY_COMPARE(window.document()->bodies().size(), std::size_t{1});
    QCOMPARE(window.document()->bodies()[0].id, sphere);
    QCOMPARE(QString::fromStdString(window.document()->undoLabel()), QStringLiteral("Delete Body"));

    // Clicking beside the entries opens no menu, so nothing is left waiting.
    emit tree->customContextMenuRequested(QPoint(5, tree->viewport()->height() - 5));
    QVERIFY(QApplication::activePopupWidget() == nullptr);
}

void ApplicationTest::selectionIsSharedBetweenBrowserAndViewport() {
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    create(window, PrimitiveKind::Box,
           {{QStringLiteral("sizeX"), 100.0},
            {QStringLiteral("sizeY"), 60.0},
            {QStringLiteral("sizeZ"), 40.0}});
    create(window, PrimitiveKind::Sphere, {{QStringLiteral("positionX"), 300.0}});
    const ObjectId box = window.document()->bodies()[0].id;
    const ObjectId sphere = window.document()->bodies()[1].id;
    auto* viewport = window.viewport();
    QVERIFY(!action(window, "edit.delete")->isEnabled());

    // Browser to viewport.
    window.browser()->bodyItem(sphere)->setSelected(true);
    QCOMPARE(viewport->selection(),
             (std::vector<SelectedItem>{{sphere, SelectedItem::Kind::Body, -1}}));
    QVERIFY(action(window, "edit.delete")->isEnabled());

    // Viewport to browser: with the Bodies filter, clicking the box selects its entry.
    auto* filter = window.findChild<QComboBox*>(QStringLiteral("selectionFilter"));
    QCOMPARE(filter->currentText(), QStringLiteral("Faces, Edges, Vertices"));
    // Choosing from the box, as a user does, and the box following the command.
    filter->setCurrentIndex(filter->findText(QStringLiteral("Bodies")));
    emit filter->activated(filter->currentIndex());
    QCOMPARE(viewport->selectionFilter(), lcad::viewport::SelectionFilter::Bodies);
    QVERIFY(action(window, "select.bodies")->isChecked());
    QVERIFY(window.browser()->selectedBodies().empty()); // the filter change cleared it
    viewport->setStandardView(StandardView::Top);
    viewport->grabFramebuffer();
    const QPoint onTheBox = viewport->project(50.0, 30.0, 40.0).toPoint();
    QTest::mouseMove(viewport, onTheBox);
    viewport->grabFramebuffer();
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, onTheBox);
    viewport->grabFramebuffer();
    QCOMPARE(window.browser()->selectedBodies(), (std::vector<ObjectId>{box}));
    QVERIFY(window.browser()->bodyItem(box)->isSelected());
    QVERIFY(!window.browser()->bodyItem(sphere)->isSelected());

    // A hidden body can still be selected in the browser, and deleted from there. The box is
    // selected at this point; choosing the sphere's entry replaces that in one step.
    window.document()->setBodyVisible(sphere, false);
    QCOMPARE(viewport->selection().size(), std::size_t{1});
    window.browser()->tree()->setCurrentItem(window.browser()->bodyItem(sphere), 0,
                                             QItemSelectionModel::ClearAndSelect);
    QVERIFY(viewport->selection().empty());
    QCOMPARE(window.browser()->selectedBodies(), (std::vector<ObjectId>{sphere}));
    QVERIFY(action(window, "edit.delete")->isEnabled());

    // Picking in the viewport replaces the selection everywhere, the hidden body included.
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, onTheBox);
    viewport->grabFramebuffer();
    QCOMPARE(window.browser()->selectedBodies(), (std::vector<ObjectId>{box}));
}

void ApplicationTest::statusBarMeasuresWhatIsPicked() {
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    create(window, PrimitiveKind::Box,
           {{QStringLiteral("sizeX"), 100.0},
            {QStringLiteral("sizeY"), 60.0},
            {QStringLiteral("sizeZ"), 40.5}});
    auto* viewport = window.viewport();
    auto* label = window.findChild<QLabel*>(QStringLiteral("selectionSummary"));
    viewport->setStandardView(StandardView::Top);
    viewport->grabFramebuffer();
    const auto click = [viewport](const QPointF& where, Qt::KeyboardModifiers modifiers = {}) {
        QTest::mouseMove(viewport, where.toPoint());
        viewport->grabFramebuffer();
        QTest::mouseClick(viewport, Qt::LeftButton, modifiers, where.toPoint());
        viewport->grabFramebuffer();
    };
    QVERIFY(label->text().isEmpty());

    click(viewport->project(50.0, 30.0, 40.5));
    QCOMPARE(window.selectionSummary(), QStringLiteral("Face of Box · area 6000 mm²"));
    QCOMPARE(label->text(), window.selectionSummary());

    click(viewport->project(50.0, 0.0, 40.5));
    QCOMPARE(window.selectionSummary(), QStringLiteral("Edge of Box · length 100 mm"));

    click(viewport->project(100.0, 60.0, 40.5));
    QCOMPARE(window.selectionSummary(), QStringLiteral("Vertex of Box · X 100 · Y 60 · Z 40.5 mm"));

    click(viewport->project(50.0, 30.0, 40.5), Qt::ShiftModifier);
    QCOMPARE(window.selectionSummary(), QStringLiteral("2 items selected"));

    window.browser()->bodyItem(window.document()->bodies()[0].id)->setSelected(true);
    QCOMPARE(window.selectionSummary(), QStringLiteral("Box · volume 243000 mm³ · area 24960 mm²"));

    viewport->clearSelection();
    QVERIFY(label->text().isEmpty());
}

void ApplicationTest::deleteRemovesTheSelectedBodiesInOneStep() {
    MainWindow window;
    create(window, PrimitiveKind::Box);
    create(window, PrimitiveKind::Sphere);
    create(window, PrimitiveKind::Cone);
    const auto& bodies = window.document()->bodies();
    QAction* remove = action(window, "edit.delete");

    // Nothing selected: nothing happens.
    remove->trigger();
    QCOMPARE(bodies.size(), std::size_t{3});

    window.browser()->bodyItem(bodies[0].id)->setSelected(true);
    window.browser()->bodyItem(bodies[2].id)->setSelected(true);
    remove->trigger();

    QCOMPARE(bodies.size(), std::size_t{1});
    QCOMPARE(bodies[0].name, std::string("Sphere"));
    QCOMPARE(QString::fromStdString(window.document()->undoLabel()),
             QStringLiteral("Delete 2 Bodies"));
    QVERIFY(!remove->isEnabled());

    action(window, "edit.undo")->trigger();
    QCOMPARE(bodies.size(), std::size_t{3});
    QCOMPARE(bodies[0].name, std::string("Box"));
    QCOMPARE(bodies[2].name, std::string("Cone"));
}

void ApplicationTest::viewMenuDrivesTheCamera() {
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    create(window, PrimitiveKind::Box);
    auto* viewport = window.viewport();
    viewport->grabFramebuffer();
    const auto looksAlong = [viewport](float x, float y, float z) {
        return (viewport->viewDirection() - QVector3D(x, y, z)).length() < 1e-3F;
    };

    action(window, "view.top")->trigger();
    QVERIFY(looksAlong(0.0F, 0.0F, -1.0F));
    action(window, "view.front")->trigger();
    QVERIFY(looksAlong(0.0F, 1.0F, 0.0F));
    action(window, "view.right")->trigger();
    QVERIFY(looksAlong(-1.0F, 0.0F, 0.0F));
    action(window, "view.home")->trigger();
    QVERIFY(viewport->viewDirection().z() < -0.5F);

    QAction* perspective = action(window, "view.perspective");
    QVERIFY(perspective->isCheckable() && !perspective->isChecked());
    perspective->trigger();
    QCOMPARE(viewport->projection(), lcad::viewport::Projection::Perspective);
    perspective->trigger();
    QCOMPARE(viewport->projection(), lcad::viewport::Projection::Orthographic);

    // Fit brings back a view that was panned away.
    const QImage framed = viewport->grabFramebuffer();
    QTest::mousePress(viewport, Qt::MiddleButton, Qt::NoModifier, QPoint(300, 300));
    viewport->grabFramebuffer();
    QMouseEvent move(QEvent::MouseMove, QPoint(500, 120), viewport->mapToGlobal(QPoint(500, 120)),
                     Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &move);
    viewport->grabFramebuffer();
    QTest::mouseRelease(viewport, Qt::MiddleButton, Qt::NoModifier, QPoint(500, 120));
    QVERIFY(differingFraction(framed, viewport->grabFramebuffer()) > 0.02);
    action(window, "view.fit")->trigger();
    QTRY_VERIFY(differingFraction(framed, viewport->grabFramebuffer()) < 0.002);
}

void ApplicationTest::zoomDirectionIsRemembered() {
    {
        MainWindow window;
        QVERIFY(!window.viewport()->isZoomReversed());
        action(window, "view.reverseZoom")->trigger();
        QVERIFY(window.viewport()->isZoomReversed());
    }
    // The next window, as after a restart, starts with the choice made in the last.
    MainWindow window;
    QVERIFY(action(window, "view.reverseZoom")->isChecked());
    QVERIFY(window.viewport()->isZoomReversed());
    action(window, "view.reverseZoom")->trigger();
    QVERIFY(!window.viewport()->isZoomReversed());
}

void ApplicationTest::closingWithUnsavedChangesAsksFirst() {
    const QString path = m_directory.filePath(QStringLiteral("unsaved.lcad"));
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    // Nothing to lose: closes without a question.
    QVERIFY(window.close());
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    create(window, PrimitiveKind::Box);
    // The window was closed and shown again, which gives it a new native window. The viewport
    // has to carry on drawing in it.
    QTRY_VERIFY(drawnFraction(window.viewport()->grabFramebuffer(),
                              window.viewport()->backgroundColor()) > 0.10);
    QString asked;
    answerNextMessageBox(QMessageBox::Cancel, &asked);
    QVERIFY(!window.close());
    QVERIFY(window.isVisible());
    QCOMPARE(asked, QStringLiteral("Save the changes to \"Untitled\" before closing it?"));
    QCOMPARE(window.document()->bodies().size(), std::size_t{1});

    // New does not need to ask: it opens another tab and leaves this document alone.
    QVERIFY(window.fileNew());
    QCOMPARE(window.documentCount(), 2);
    window.closeDocument(1);
    QCOMPARE(window.documentCount(), 1);
    QCOMPARE(window.document()->bodies().size(), std::size_t{1});

    // Save: the document has a file, so it is saved there and the window closes.
    QVERIFY(window.saveTo(path));
    create(window, PrimitiveKind::Sphere);
    QVERIFY(window.isWindowModified());
    answerNextMessageBox(QMessageBox::Save);
    QVERIFY(window.close());
    QCOMPARE(lcad::document::loadDocument(std::filesystem::path(path.toStdString()))
                 .document->bodies()
                 .size(),
             std::size_t{2});

    // Discard: closes, and the file keeps what was last saved.
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    create(window, PrimitiveKind::Cone);
    answerNextMessageBox(QMessageBox::Discard);
    QVERIFY(window.close());
    QCOMPARE(lcad::document::loadDocument(std::filesystem::path(path.toStdString()))
                 .document->bodies()
                 .size(),
             std::size_t{2});
}

void ApplicationTest::aFileThatCannotBeOpenedLeavesTheDocumentInPlace() {
    const QString notADocument = m_directory.filePath(QStringLiteral("notes.lcad"));
    QFile file(notADocument);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("just some text\n");
    file.close();
    MainWindow window;
    create(window, PrimitiveKind::Box);
    const auto* document = window.document();

    QString error;
    QVERIFY(!window.loadFrom(notADocument, &error));
    QVERIFY2(error.contains(QStringLiteral("not an LCAD document")), qPrintable(error));
    QVERIFY(!window.loadFrom(m_directory.filePath(QStringLiteral("missing.lcad")), &error));
    QVERIFY2(error.contains(QStringLiteral("There is no file")), qPrintable(error));

    QCOMPARE(window.document(), document);
    QCOMPARE(window.document()->bodies().size(), std::size_t{1});
    QVERIFY(window.filePath().isEmpty());
    QVERIFY(window.isWindowModified());
}

void ApplicationTest::savingToAnImpossiblePlaceReportsIt() {
    MainWindow window;
    create(window, PrimitiveKind::Box);

    QString error;
    QVERIFY(
        !window.saveTo(m_directory.filePath(QStringLiteral("no-such-folder/part.lcad")), &error));
    QVERIFY2(error.contains(QStringLiteral("does not exist")), qPrintable(error));
    // Still unsaved, and still without a file.
    QVERIFY(window.isWindowModified());
    QVERIFY(window.filePath().isEmpty());
}

void ApplicationTest::logGoesToAFile() {
    const QString path = m_directory.filePath(QStringLiteral("state/lcad/lcad.log"));
    QVERIFY(lcad::app::logFilePath().isEmpty());

    lcad::app::startLogging(path);
    QCOMPARE(lcad::app::logFilePath(), path);
    lcad::core::log(lcad::core::LogLevel::Info, "document", "opened bracket.lcad");
    // Qt's own messages take the same route. Not a warning: those fail the test on purpose.
    qInfo("a note from Qt");
    {
        const MainWindow window;
        QVERIFY(window.aboutText().contains(path));
    }
    lcad::app::stopLogging();
    lcad::core::log(lcad::core::LogLevel::Info, "document", "after logging stopped");

    QVERIFY(lcad::app::logFilePath().isEmpty());
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(file.readAll());
    QVERIFY2(text.contains(QStringLiteral("info    [app] LCAD %1 started logging")
                               .arg(lcad::app::versionString())),
             qPrintable(text));
    QVERIFY(text.contains(QStringLiteral("info    [document] opened bracket.lcad")));
    QVERIFY(text.contains(QStringLiteral("info    [qt] a note from Qt")));
    QVERIFY(!text.contains(QStringLiteral("after logging stopped")));
    // Every line starts with the time it was written.
    QVERIFY(QRegularExpression(QStringLiteral(R"(^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3} )"))
                .match(text)
                .hasMatch());
}

QTEST_MAIN(ApplicationTest)
#include "ApplicationTest.moc"
