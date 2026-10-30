// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/BrowserPanel.h>
#include <lcad/app/Commands.h>
#include <lcad/app/MainWindow.h>
#include <lcad/app/PropertiesPanel.h>
#include <lcad/app/SketchPanel.h>
#include <lcad/app/Theme.h>
#include <lcad/document/SketchShapes.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Plane.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/sketch/Edit.h>
#include <lcad/viewport/SketchEditor.h>
#include <lcad/viewport/ViewportWidget.h>

#include <FrameChecks.h>
#include <GraphicsMessages.h>
#include <GuiHelpers.h>

#include <QCheckBox>
#include <QDockWidget>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>

#include <cmath>
#include <memory>

using lcad::app::MainWindow;
using lcad::app::PrimitiveKind;
using lcad::app::Theme;
using lcad::app::ThemeChoice;
using lcad::document::ObjectId;
using lcad::document::SketchItem;
using lcad::sketch::Constraint;
using lcad::sketch::ConstraintId;
using lcad::sketch::ConstraintKind;
using lcad::sketch::Entity;
using lcad::sketch::EntityId;
using lcad::sketch::EntityKind;
using lcad::sketch::Point2;
using lcad::test::entriesOf;
using lcad::test::keepFrame;
using lcad::viewport::SelectedItem;
using lcad::viewport::SketchEditor;
using lcad::viewport::SketchTool;
using lcad::viewport::StandardView;

namespace {

bool sameDirection(const QVector3D& a, const QVector3D& b) {
    return (a - b).length() < 1e-3F;
}

double apart(Point2 a, Point2 b) {
    return std::hypot(b.x - a.x, b.y - a.y);
}

} // namespace

// Sketching as the whole window offers it: started from a command, drawn in the 3D view with
// the tools of the sketch toolbar, constrained and dimensioned, finished, saved, and opened
// again. The model is checked in the document; the window is checked for showing it.
class SketchInterfaceTest : public QObject {
    Q_OBJECT

public:
    static void initMain() {
        lcad::viewport::prepareApplication();
        qunsetenv("XDG_CURRENT_DESKTOP");
        static QTemporaryDir settingsDirectory;
        QCoreApplication::setOrganizationName(QStringLiteral("LCAD"));
        QCoreApplication::setApplicationName(QStringLiteral("LCAD"));
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDirectory.path());
    }

private slots:
    void init();
    void cleanup();

    void aNewSketchAsksForItsPlane();
    void aNewSketchGoesOnTheFlatFaceThatIsSelected();
    void theWindowChangesForSketchingAndBack();
    void sketchToolsAreCommandsWithShortcuts();
    void aRectangleIsDrawnConstrainedDimensionedSavedAndReopened();
    void escapeStepsBackAndDeleteRemovesWhatIsSelected();
    void thePanelShowsTheStateAndTheConstraints();
    void theBrowserListsSketches();
    void sketchesAreHiddenShownRenamedAndDeleted();
    void theContextMenuInASketchIsAboutTheSketch();
    void anotherTabOrClosingLeavesTheSketch();
    void theSketchToolbarFitsAndDrawsInBothThemes_data();
    void theSketchToolbarFitsAndDrawsInBothThemes();

private:
    static QAction* action(const MainWindow& window, const char* id) {
        return window.commands()->action(QString::fromLatin1(id));
    }
    static bool trigger(const MainWindow& window, const char* id) {
        return window.commands()->trigger(QString::fromLatin1(id));
    }
    static void show(MainWindow& window) {
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
    }
    static SketchEditor* editor(const MainWindow& window) {
        return window.viewport()->sketchEditor();
    }
    static void render(const MainWindow& window) { window.viewport()->grabFramebuffer(); }
    // Where a position of the sketch being edited is in the 3D view.
    static QPoint at(const MainWindow& window, double u, double v) {
        return editor(window)->toWidget({u, v}).toPoint();
    }
    // The middle of an origin plane's patch in the 3D view, while a plane is being chosen: 120
    // pixels along each axis as seen square on, reaching toward the side the view is seen from.
    static QPoint onPlane(const MainWindow& window, lcad::viewport::OriginItem plane) {
        using lcad::viewport::OriginItem;
        const auto* viewport = window.viewport();
        const QVector3D looking = viewport->viewDirection();
        const auto toward = [&looking](const QVector3D& axis) {
            return QVector3D::dotProduct(looking, axis) > 1e-6F ? -axis : axis;
        };
        const QVector3D u =
            toward(plane == OriginItem::PlaneYZ ? QVector3D(0, 1, 0) : QVector3D(1, 0, 0));
        const QVector3D v =
            toward(plane == OriginItem::PlaneXY ? QVector3D(0, 1, 0) : QVector3D(0, 0, 1));
        const double pixelsPerMillimeter = viewport->height() / viewport->cameraState().scale;
        const QPointF origin = viewport->project(0.0, 0.0, 0.0);
        const auto onScreen = [&](const QVector3D& axis) {
            return (viewport->project(axis.x(), axis.y(), axis.z()) - origin) /
                   pixelsPerMillimeter * 120.0;
        };
        return (origin + (onScreen(u) + onScreen(v)) * 0.5).toPoint();
    }
    static void click(const MainWindow& window, const QPoint& point,
                      Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QTest::mouseMove(window.viewport(), point);
        render(window);
        QTest::mouseClick(window.viewport(), Qt::LeftButton, modifiers, point);
        render(window);
    }
    // Starts a sketch on the XY plane and gives it a view 200 mm tall around (50, 30).
    static ObjectId startSketch(MainWindow& window) {
        const ObjectId id = window.createSketch(lcad::kernel::planeXY());
        render(window);
        lcad::viewport::CameraState camera = window.viewport()->cameraState();
        camera.center = {50.0, 30.0, 0.0};
        camera.eye = {50.0, 30.0, 500.0};
        camera.up = {0.0, 1.0, 0.0};
        camera.scale = 200.0;
        window.viewport()->setCameraState(camera);
        render(window);
        return id;
    }
    static std::vector<Entity> lines(const lcad::sketch::Sketch& geometry) {
        std::vector<Entity> found;
        for (const Entity& entity : geometry.entities()) {
            if (entity.kind == EntityKind::Line) {
                found.push_back(entity);
            }
        }
        return found;
    }

    QTemporaryDir m_directory;
    lcad::test::GraphicsMessages m_graphicsMessages;
};

void SketchInterfaceTest::init() {
    QTest::failOnWarning(QRegularExpression(QStringLiteral("OpenGL|QRhi|context|surface"),
                                            QRegularExpression::CaseInsensitiveOption));
    QSettings().clear();
    Theme::instance().load();
    QVERIFY(m_directory.isValid());
}

void SketchInterfaceTest::cleanup() {
    const QStringList complaints = m_graphicsMessages.take();
    QVERIFY2(complaints.isEmpty(), qPrintable(complaints.join(QStringLiteral(" | "))));
}

void SketchInterfaceTest::aNewSketchAsksForItsPlane() {
    using lcad::viewport::OriginItem;
    MainWindow window;
    show(window);
    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("propertiesDock"));
    auto* viewport = window.viewport();
    QVERIFY(action(window, "sketch.new")->isEnabled());
    QVERIFY(!action(window, "sketch.finish")->isEnabled());
    QVERIFY(!viewport->isPlanePicking());

    // New Sketch, with nothing selected: the three planes appear in the 3D view to be clicked,
    // and the panel says so.
    QVERIFY(trigger(window, "sketch.new"));
    QVERIFY(window.planePanel() != nullptr);
    QVERIFY(window.planePanel()->isVisible());
    QCOMPARE(dock->windowTitle(), QStringLiteral("New Sketch"));
    QVERIFY(window.commandText().contains(QStringLiteral("plane")));
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("3D view")));
    QVERIFY(window.planePanel()
                ->findChild<QLabel*>(QStringLiteral("sketchPlaneHint"))
                ->text()
                .contains(QStringLiteral("3D view")));
    QVERIFY(viewport->isPlanePicking());
    render(window);
    for (const OriginItem plane : {OriginItem::PlaneXY, OriginItem::PlaneXZ, OriginItem::PlaneYZ}) {
        const auto found = viewport->pickPlaneAt(onPlane(window, plane));
        QVERIFY(found.has_value());
        QCOMPARE(static_cast<int>(*found), static_cast<int>(plane));
        // Each has a button too, with a swatch in the plane's color.
        QVERIFY(!window.planePanel()->planeButton(static_cast<int>(plane))->icon().isNull());
    }
    QVERIFY(window.document()->sketches().empty());

    // Pointing at a plane says which it is: in the status bar, and by marking its button.
    QTest::mouseMove(viewport, onPlane(window, OriginItem::PlaneXZ));
    render(window);
    QTRY_COMPARE(window.planePanel()->hoveredPlane(), 1);
    QVERIFY(window.statusBar()->currentMessage().startsWith(QStringLiteral("XZ Plane")));
    QVERIFY(window.planePanel()->planeButton(1)->property("planeHovered").toBool());
    render(window);
    keepFrame(window.grab().toImage(), "sketch-window-choose-plane");
    QVERIFY(!window.planePanel()->planeButton(0)->property("planeHovered").toBool());

    // Esc gives it up: nothing was made, and the planes go away again.
    QVERIFY(trigger(window, "edit.cancel"));
    QTRY_VERIFY(window.planePanel() == nullptr);
    QVERIFY(!viewport->isPlanePicking());
    QVERIFY(window.document()->sketches().empty());
    QVERIFY(!window.isSketching());
    QCOMPARE(dock->windowTitle(), QStringLiteral("Properties"));

    // Again, and click the XZ plane in the 3D view: the sketch exists, is open, and is seen
    // from the front.
    QVERIFY(trigger(window, "sketch.new"));
    render(window);
    click(window, onPlane(window, OriginItem::PlaneXZ));
    QTRY_COMPARE(window.document()->sketches().size(), std::size_t{1});
    QVERIFY(window.planePanel() == nullptr);
    QVERIFY(!viewport->isPlanePicking());
    const SketchItem& sketch = window.document()->sketches().front();
    QCOMPARE(QString::fromStdString(sketch.name), QStringLiteral("Sketch 1"));
    QCOMPARE(sketch.plane.normal.y, -1.0);
    QVERIFY(window.isSketching());
    QCOMPARE(viewport->editedSketch(), sketch.id);
    render(window);
    QVERIFY(sameDirection(viewport->viewDirection(), {0.0F, 1.0F, 0.0F}));
    // The click chose a plane. It did not select anything, or draw anything.
    QVERIFY(viewport->selection().empty());
    QCOMPARE(sketch.geometry.entities().size(), std::size_t{1});
    // One undo step, which takes the sketch away again and closes it.
    QCOMPARE(QString::fromStdString(window.document()->undoLabel()),
             QStringLiteral("Create Sketch"));
    QVERIFY(trigger(window, "edit.undo"));
    QVERIFY(window.document()->sketches().empty());
    QVERIFY(!window.isSketching());

    // The buttons in the panel do the same, for a plane that is hard to click: seen from the
    // top, the other two are edge-on.
    viewport->setStandardView(StandardView::Top);
    render(window);
    QVERIFY(trigger(window, "sketch.new"));
    render(window);
    QTest::mouseClick(window.planePanel()->planeButton(2), Qt::LeftButton);
    QTRY_VERIFY(window.planePanel() == nullptr);
    QCOMPARE(window.document()->sketches().front().plane.normal.x, 1.0);
    QVERIFY(window.isSketching());
    QVERIFY(!viewport->isPlanePicking());
    // While a sketch is open there is no starting another.
    QVERIFY(!action(window, "sketch.new")->isEnabled());
    QVERIFY(!trigger(window, "sketch.new.xy"));
    window.finishSketch();

    // The planes are commands of their own, too.
    QVERIFY(trigger(window, "sketch.new.xy"));
    QCOMPARE(window.document()->sketches().size(), std::size_t{2});
    QCOMPARE(window.document()->sketches().back().plane.normal.z, 1.0);
    QVERIFY(window.isSketching());
}

void SketchInterfaceTest::aNewSketchGoesOnTheFlatFaceThatIsSelected() {
    MainWindow window;
    show(window);
    window.startPrimitive(PrimitiveKind::Box);
    window.activePanel()->setValue(QStringLiteral("sizeX"), 100.0);
    window.activePanel()->setValue(QStringLiteral("sizeY"), 60.0);
    window.activePanel()->setValue(QStringLiteral("sizeZ"), 40.0);
    window.acceptCommand();
    window.startPrimitive(PrimitiveKind::Cylinder);
    window.activePanel()->setValue(QStringLiteral("positionX"), 250.0);
    window.acceptCommand();
    auto* viewport = window.viewport();
    viewport->setStandardView(StandardView::Top);
    render(window);

    // Click the top of the box, then New Sketch: no question asked.
    click(window, viewport->project(50.0, 30.0, 40.0).toPoint());
    QCOMPARE(viewport->selection().size(), std::size_t{1});
    QVERIFY(trigger(window, "sketch.new"));
    QVERIFY(window.planePanel() == nullptr);
    QVERIFY(window.isSketching());
    QCOMPARE(window.document()->sketches().size(), std::size_t{1});
    const lcad::kernel::Plane plane = window.document()->sketches()[0].plane;
    QVERIFY(qAbs(plane.normal.z - 1.0) < 1e-9);
    QVERIFY(qAbs(plane.origin.z - 40.0) < 1e-9);

    // What is drawn there is at the height of the face.
    render(window);
    trigger(window, "view.fit");
    render(window);
    QVERIFY(trigger(window, "sketch.circle"));
    const QPoint center = viewport->project(50.0, 30.0, 40.0).toPoint();
    click(window, center);
    click(window, center + QPoint(30, 0));
    window.finishSketch();
    const SketchItem& made = window.document()->sketches()[0];
    const auto bounds = lcad::document::sketchBounds(made);
    QVERIFY(bounds.has_value());
    QVERIFY(qAbs(bounds->min.z - 40.0) < 1e-9 && qAbs(bounds->max.z - 40.0) < 1e-9);
    QVERIFY(qAbs((bounds->min.x + bounds->max.x) / 2.0 - 50.0) < 1.0);

    // With the question open, a flat face that is clicked answers it. A round one does not.
    viewport->setStandardView(StandardView::Front);
    render(window);
    QVERIFY(trigger(window, "sketch.new"));
    QVERIFY(window.planePanel() != nullptr);
    // The side of the cylinder, seen from the front.
    click(window, viewport->project(250.0, -25.0, 25.0).toPoint());
    QCOMPARE(window.document()->sketches().size(), std::size_t{1});
    QVERIFY(window.planePanel() != nullptr);
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("not flat")));
    // The front of the box.
    click(window, viewport->project(50.0, 0.0, 20.0).toPoint());
    QTRY_COMPARE(window.document()->sketches().size(), std::size_t{2});
    QVERIFY(window.isSketching());
    QVERIFY(qAbs(window.document()->sketches()[1].plane.normal.y + 1.0) < 1e-9);
}

void SketchInterfaceTest::theWindowChangesForSketchingAndBack() {
    MainWindow window;
    show(window);
    auto* modeling = window.findChild<QToolBar*>(QStringLiteral("modelingToolbar"));
    auto* sketching = window.findChild<QToolBar*>(QStringLiteral("sketchToolbar"));
    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("propertiesDock"));
    QVERIFY(modeling != nullptr && sketching != nullptr);
    QVERIFY(modeling->isVisible());
    QVERIFY(!sketching->isVisible());
    window.viewport()->setStandardView(StandardView::Home);
    render(window);
    const QVector3D home = window.viewport()->viewDirection();
    dock->hide();

    const ObjectId id = startSketch(window);
    // The sketch tools take the toolbar, the panel on the right is about the sketch, and the
    // status bar says what is open.
    QVERIFY(!modeling->isVisible());
    QVERIFY(sketching->isVisible());
    QVERIFY(dock->isVisible());
    QCOMPARE(dock->windowTitle(), QStringLiteral("Sketch"));
    QVERIFY(window.sketchPanel()->isVisible());
    QVERIFY(!window.properties()->isVisible());
    QCOMPARE(window.sketchPanel()->heading(), QStringLiteral("Sketch 1"));
    QCOMPARE(window.selectionSummary(), QStringLiteral("Sketch 1 · Nothing drawn yet"));
    // The drawing tools can be used; the modeling ones cannot.
    QVERIFY(action(window, "sketch.line")->isEnabled());
    QVERIFY(action(window, "sketch.finish")->isEnabled());
    QVERIFY(!action(window, "create.box")->isEnabled());
    QVERIFY(!action(window, "select.faces")->isEnabled());
    // There is a plain way out, on the toolbar and in the panel.
    auto* finish = sketching->findChild<QToolButton*>(QStringLiteral("finishSketch"));
    QVERIFY(finish != nullptr && finish->isVisible());
    QVERIFY(finish->text().contains(QStringLiteral("Finish")));
    QVERIFY(window.sketchPanel()->finishButton()->isVisible());
    // The browser shows which sketch is open.
    QTreeWidgetItem* entry = window.browser()->sketchItem(id);
    QVERIFY(entry != nullptr);
    QVERIFY(entry->font(0).bold());
    QVERIFY(entry->isSelected());

    // Finish, by the button on the toolbar: everything is as it was for modeling.
    QTest::mouseClick(finish, Qt::LeftButton);
    QVERIFY(!window.isSketching());
    QVERIFY(modeling->isVisible());
    QVERIFY(!sketching->isVisible());
    QCOMPARE(dock->windowTitle(), QStringLiteral("Properties"));
    QVERIFY(window.properties()->isVisible());
    QVERIFY(!entry->font(0).bold());
    QVERIFY(action(window, "create.box")->isEnabled());
    QVERIFY(!action(window, "sketch.line")->isEnabled());
    render(window);
    QVERIFY(sameDirection(window.viewport()->viewDirection(), home));

    // The panel's button does the same.
    QVERIFY(window.editSketch(id));
    QVERIFY(window.isSketching());
    QTest::mouseClick(window.sketchPanel()->finishButton(), Qt::LeftButton);
    QVERIFY(!window.isSketching());
    // And so does Ctrl + Enter.
    QVERIFY(window.editSketch(id));
    QCOMPARE(action(window, "sketch.finish")->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_Return));
    QVERIFY(trigger(window, "sketch.finish"));
    QVERIFY(!window.isSketching());

    // Giving the window to the 3D view hides whichever toolbar is in use, and brings it back.
    QVERIFY(window.editSketch(id));
    window.setViewportFocus(true);
    QVERIFY(!sketching->isVisible() && !modeling->isVisible());
    window.setViewportFocus(false);
    QVERIFY(sketching->isVisible() && !modeling->isVisible());
    window.finishSketch();
    QVERIFY(modeling->isVisible() && !sketching->isVisible());
}

void SketchInterfaceTest::sketchToolsAreCommandsWithShortcuts() {
    MainWindow window;
    show(window);
    // Outside a sketch the tools cannot be started, by any means.
    for (const char* id : {"sketch.line", "sketch.rectangle", "sketch.circle", "sketch.arc",
                           "sketch.point", "sketch.polygon", "sketch.slot", "sketch.dimension",
                           "sketch.trim", "sketch.extend", "sketch.offset", "sketch.mirror",
                           "sketch.construction", "sketch.parallel", "sketch.fix"}) {
        QVERIFY2(!action(window, id)->isEnabled(), id);
        QVERIFY2(!trigger(window, id), id);
        QVERIFY2(!action(window, id)->toolTip().isEmpty(), id);
    }
    startSketch(window);
    SketchEditor* sketch = editor(window);

    // Each tool is a command; exactly one of them is on.
    const std::pair<const char*, SketchTool> tools[] = {
        {"sketch.line", SketchTool::Line},           {"sketch.rectangle", SketchTool::Rectangle},
        {"sketch.circle", SketchTool::Circle},       {"sketch.arc", SketchTool::Arc},
        {"sketch.point", SketchTool::Point},         {"sketch.polygon", SketchTool::Polygon},
        {"sketch.slot", SketchTool::Slot},           {"sketch.trim", SketchTool::Trim},
        {"sketch.extend", SketchTool::Extend},       {"sketch.offset", SketchTool::Offset},
        {"sketch.dimension", SketchTool::Dimension}, {"sketch.select", SketchTool::Select},
    };
    for (const auto& [id, tool] : tools) {
        QVERIFY2(trigger(window, id), id);
        QCOMPARE(sketch->tool(), tool);
        int checked = 0;
        for (const auto& [other, unused] : tools) {
            Q_UNUSED(unused)
            checked += action(window, other)->isChecked() ? 1 : 0;
        }
        QVERIFY2(action(window, id)->isChecked() && checked == 1, id);
        // The status bar says which tool it is and what it waits for.
        if (tool != SketchTool::Select) {
            QVERIFY2(!window.commandText().isEmpty(), id);
        }
    }
    QVERIFY(window.commandText().isEmpty());

    // The keys a Fusion user reaches for, pressed for real: shortcuts work in the window that
    // has the keyboard.
    window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    window.viewport()->setFocus();
    const std::pair<Qt::Key, SketchTool> keys[] = {
        {Qt::Key_L, SketchTool::Line},      {Qt::Key_R, SketchTool::Rectangle},
        {Qt::Key_C, SketchTool::Circle},    {Qt::Key_A, SketchTool::Arc},
        {Qt::Key_D, SketchTool::Dimension}, {Qt::Key_T, SketchTool::Trim},
        {Qt::Key_O, SketchTool::Offset},
    };
    for (const auto& [key, tool] : keys) {
        QTest::keyClick(&window, key);
        QCOMPARE(sketch->tool(), tool);
    }
    // Esc puts the tool away.
    QTest::keyClick(&window, Qt::Key_Escape);
    QCOMPARE(sketch->tool(), SketchTool::Select);

    // A constraint with nothing selected becomes the tool, and shows as on.
    QVERIFY(trigger(window, "sketch.parallel"));
    QCOMPARE(sketch->tool(), SketchTool::Constraint);
    QVERIFY(action(window, "sketch.parallel")->isChecked());
    QVERIFY(!action(window, "sketch.perpendicular")->isChecked());
    QVERIFY(window.commandText().contains(QStringLiteral("Parallel")));
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("two lines")));
    QTest::keyClick(&window, Qt::Key_Escape);
    QVERIFY(!action(window, "sketch.parallel")->isChecked());

    // X switches construction on and off; the inference switch is a command as well.
    QVERIFY(!sketch->constructionMode());
    QTest::keyClick(&window, Qt::Key_X);
    QVERIFY(sketch->constructionMode());
    QVERIFY(action(window, "sketch.construction")->isChecked());
    QTest::keyClick(&window, Qt::Key_X);
    QVERIFY(!sketch->constructionMode());
    QVERIFY(action(window, "sketch.inference")->isChecked());
    QVERIFY(trigger(window, "sketch.inference"));
    QVERIFY(!sketch->hasInference());
    QVERIFY(!action(window, "sketch.inference")->isChecked());

    // The search finds the tools by other words for them.
    const auto finds = [&window](const QString& typed, const char* id) {
        for (const lcad::app::CommandMatch& match : window.commands()->search(typed, 12)) {
            if (match.id == QLatin1String(id)) {
                return true;
            }
        }
        return false;
    };
    QVERIFY(finds(QStringLiteral("hexagon"), "sketch.polygon"));
    QVERIFY(finds(QStringLiteral("measure"), "sketch.dimension"));
    QVERIFY(finds(QStringLiteral("snap"), "sketch.inference"));
    QVERIFY(finds(QStringLiteral("done"), "sketch.finish"));
}

// The acceptance test of Phase 2, through the whole window, and steps 4 to 7 of the prototype
// the interface specification asks for: create a sketch on a reference plane, draw and
// constrain a rectangle, enter exact dimensions, finish the sketch. Then what Phase 1 asked of
// every document: save, close, reopen, and go on editing.
void SketchInterfaceTest::aRectangleIsDrawnConstrainedDimensionedSavedAndReopened() {
    const QString path = m_directory.filePath(QStringLiteral("plate.lcad"));
    ObjectId id;
    {
        MainWindow window;
        show(window);
        // 4. A sketch on a reference plane.
        id = startSketch(window);
        // 5. Draw a rectangle from the origin. Its corners join, its sides are level and
        // upright, and its first corner stays on the origin: the drawing itself constrains it.
        QVERIFY(trigger(window, "sketch.rectangle"));
        click(window, at(window, 0.0, 0.0));
        click(window, at(window, 80.0, 50.0));
        const lcad::sketch::Sketch& drawn = window.document()->findSketch(id)->geometry;
        QCOMPARE(lines(drawn).size(), std::size_t{4});
        QCOMPARE(editor(window)->state().degreesOfFreedom, 2);
        QCOMPARE(window.selectionSummary(), QStringLiteral("Sketch 1 · 2 degrees of freedom"));
        QCOMPARE(window.sketchPanel()->stateText(), QStringLiteral("2 degrees of freedom"));

        // 6. Exact dimensions, typed in the view.
        QVERIFY(trigger(window, "sketch.dimension"));
        click(window, at(window, 40.0, 0.0));
        click(window, at(window, 40.0, -15.0));
        QLineEdit* field = editor(window)->valueField();
        QVERIFY(field != nullptr && field->isVisible());
        // In the view, by the line it is about, not in a dialog or a side panel.
        QCOMPARE(field->parentWidget(), window.viewport());
        QTest::keyClicks(field, QStringLiteral("120"));
        QTest::keyClick(field, Qt::Key_Return);
        QCOMPARE(window.sketchPanel()->stateText(), QStringLiteral("1 degree of freedom"));
        click(window, at(window, 120.0, 25.0));
        click(window, at(window, 135.0, 25.0));
        field = editor(window)->valueField();
        QVERIFY(field != nullptr);
        QTest::keyClicks(field, QStringLiteral("45"));
        QTest::keyClick(field, Qt::Key_Return);

        QVERIFY(editor(window)->state().isFullyConstrained());
        QCOMPARE(window.sketchPanel()->stateText(), QStringLiteral("Fully constrained"));
        QCOMPARE(window.selectionSummary().section(QStringLiteral(" · "), 0, 1),
                 QStringLiteral("Sketch 1 · Fully constrained"));
        keepFrame(window.grab().toImage(), "sketch-window-constrained");

        // 7. Finish the sketch.
        QVERIFY(trigger(window, "sketch.finish"));
        QVERIFY(!window.isSketching());
        const lcad::sketch::Sketch& finished = window.document()->findSketch(id)->geometry;
        QVERIFY(finished.state().isFullyConstrained());
        QCOMPARE(finished.profiles().size(), std::size_t{1});
        QVERIFY(qAbs(finished.profiles()[0].area - 120.0 * 45.0) < 1e-6);
        // It is still shown, among the bodies, and can be picked there.
        render(window);
        window.viewport()->setStandardView(StandardView::Top);
        render(window);
        click(window, window.viewport()->project(60.0, 0.0, 0.0).toPoint());
        QCOMPARE(window.viewport()->selection(),
                 (std::vector<SelectedItem>{{id, SelectedItem::Kind::Sketch, -1}}));
        QCOMPARE(window.selectionSummary(), QStringLiteral("Sketch 1 · Fully constrained"));
        QCOMPARE(window.properties()->heading(), QStringLiteral("Sketch 1"));
        QVERIFY(window.properties()->rows().contains(QStringLiteral("Closed outlines: 1")));

        QString error;
        QVERIFY2(window.saveTo(path, &error), qPrintable(error));
        QVERIFY(!window.document()->isModified());
    }

    // Close, reopen, and go on editing.
    MainWindow window;
    show(window);
    QString error;
    QVERIFY2(window.loadFrom(path, &error), qPrintable(error));
    QVERIFY(window.loadWarnings().isEmpty());
    QCOMPARE(window.document()->sketches().size(), std::size_t{1});
    const SketchItem* reopened = window.document()->findSketch(id);
    QVERIFY(reopened != nullptr);
    QCOMPARE(QString::fromStdString(reopened->name), QStringLiteral("Sketch 1"));
    QVERIFY(reopened->geometry.state().isFullyConstrained());
    QVERIFY(qAbs(reopened->geometry.profiles()[0].area - 5400.0) < 1e-6);

    // Open it from the browser with a double-click, and change the width.
    QTreeWidget* tree = window.browser()->tree();
    QTreeWidgetItem* entry = window.browser()->sketchItem(id);
    QVERIFY(entry != nullptr);
    tree->scrollToItem(entry);
    const QPoint onEntry = tree->visualItemRect(entry).center();
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, onEntry);
    QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, onEntry);
    QVERIFY(window.isSketching());
    QCOMPARE(window.viewport()->editedSketch(), id);
    render(window);

    ConstraintId width = 0;
    for (const Constraint& constraint : editor(window)->geometry().constraints()) {
        if (constraint.kind == ConstraintKind::Distance && constraint.value == 120.0) {
            width = constraint.id;
        }
    }
    QVERIFY(width != 0);
    const QPoint onValue = editor(window)->dimensionRect(width).center().toPoint();
    QTest::mouseMove(window.viewport(), onValue);
    QTest::mouseDClick(window.viewport(), Qt::LeftButton, Qt::NoModifier, onValue);
    render(window);
    QLineEdit* field = editor(window)->valueField();
    QVERIFY(field != nullptr);
    QCOMPARE(field->text(), QStringLiteral("120"));
    QTest::keyClicks(field, QStringLiteral("75.5"));
    QTest::keyClick(field, Qt::Key_Return);

    const lcad::sketch::Sketch& edited = window.document()->findSketch(id)->geometry;
    QVERIFY(edited.state().isFullyConstrained());
    QVERIFY(qAbs(edited.profiles()[0].area - 75.5 * 45.0) < 1e-6);
    const std::vector<Entity> sides = lines(edited);
    QVERIFY(apart(edited.position(sides[0].end), {75.5, 0.0}) < 1e-6);
    QVERIFY(apart(edited.position(sides[1].end), {75.5, 45.0}) < 1e-6);
    QVERIFY(window.document()->isModified());
    // Undo and redo are the Edit menu's, as everywhere.
    QVERIFY(action(window, "edit.undo")->text().contains(QStringLiteral("Change Dimension")));
    QVERIFY(trigger(window, "edit.undo"));
    QVERIFY(qAbs(window.document()->findSketch(id)->geometry.profiles()[0].area - 5400.0) < 1e-6);
    QVERIFY(trigger(window, "edit.redo"));
    QVERIFY(qAbs(window.document()->findSketch(id)->geometry.profiles()[0].area - 75.5 * 45.0) <
            1e-6);
    window.finishSketch();
    window.document()->markSaved();
}

void SketchInterfaceTest::escapeStepsBackAndDeleteRemovesWhatIsSelected() {
    MainWindow window;
    show(window);
    const ObjectId id = startSketch(window);
    const auto geometry = [&window, id]() -> const lcad::sketch::Sketch& {
        return window.document()->findSketch(id)->geometry;
    };
    QVERIFY(!action(window, "edit.cancel")->isEnabled());
    QVERIFY(!action(window, "edit.delete")->isEnabled());

    // A line half drawn: Esc abandons it, Esc again puts the tool away. The sketch stays open.
    QVERIFY(trigger(window, "sketch.line"));
    click(window, at(window, 10.0, 10.0));
    QVERIFY(action(window, "edit.cancel")->isEnabled());
    QVERIFY(trigger(window, "edit.cancel"));
    QCOMPARE(editor(window)->tool(), SketchTool::Line);
    QVERIFY(trigger(window, "edit.cancel"));
    QCOMPARE(editor(window)->tool(), SketchTool::Select);
    QVERIFY(!action(window, "edit.cancel")->isEnabled());
    QVERIFY(window.isSketching());
    QVERIFY(lines(geometry()).empty());

    // Draw two lines; select one by clicking it; Delete removes it and nothing else.
    QVERIFY(trigger(window, "sketch.line"));
    click(window, at(window, 10.0, 10.0));
    click(window, at(window, 60.0, 30.0));
    trigger(window, "edit.cancel");
    click(window, at(window, 10.0, 50.0));
    click(window, at(window, 60.0, 60.0));
    trigger(window, "edit.cancel");
    trigger(window, "edit.cancel");
    QCOMPARE(lines(geometry()).size(), std::size_t{2});
    click(window, at(window, 35.0, 20.0));
    QCOMPARE(editor(window)->selectedEntities().size(), std::size_t{1});
    QVERIFY(window.selectionSummary().contains(QStringLiteral("Line · length")));
    QVERIFY(action(window, "edit.delete")->isEnabled());
    window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    QTest::keyClick(&window, Qt::Key_Delete);
    QCOMPARE(lines(geometry()).size(), std::size_t{1});
    QCOMPARE(window.document()->sketches().size(), std::size_t{1});
    QVERIFY(!action(window, "edit.delete")->isEnabled());
    QVERIFY(action(window, "edit.undo")->text().contains(QStringLiteral("Delete")));
    QVERIFY(trigger(window, "edit.undo"));
    QCOMPARE(lines(geometry()).size(), std::size_t{2});
}

void SketchInterfaceTest::thePanelShowsTheStateAndTheConstraints() {
    MainWindow window;
    show(window);
    const ObjectId id = startSketch(window);
    lcad::app::SketchPanel* panel = window.sketchPanel();
    const auto geometry = [&window, id]() -> const lcad::sketch::Sketch& {
        return window.document()->findSketch(id)->geometry;
    };
    QCOMPARE(panel->stateText(), QStringLiteral("Nothing drawn yet"));
    QCOMPARE(panel->selectionText(), QStringLiteral("Nothing selected"));
    QVERIFY(panel->constraintRows().isEmpty());

    QVERIFY(trigger(window, "sketch.rectangle"));
    click(window, at(window, 10.0, 10.0));
    click(window, at(window, 70.0, 50.0));
    trigger(window, "edit.cancel");
    trigger(window, "edit.cancel");
    QCOMPARE(panel->stateText(), QStringLiteral("4 degrees of freedom"));
    QVERIFY(panel->findChild<QLabel*>(QStringLiteral("sketchState")) != nullptr);

    // Select the bottom side: the panel says what it is and lists what holds it.
    click(window, at(window, 40.0, 10.0));
    // About 60 mm: where a click lands goes by the pixel, and how big a pixel is by the window.
    const QStringList saying = panel->selectionText().split(QLatin1Char(' '));
    QVERIFY2(saying.size() == 5 && saying[0] == QStringLiteral("Line") &&
                 qAbs(saying[3].toDouble() - 60.0) < 1.0,
             qPrintable(panel->selectionText()));
    QStringList rows = panel->constraintRows();
    // Level, and joined to its neighbors at both ends.
    QCOMPARE(rows.count(QStringLiteral("Horizontal")), 1);
    QCOMPARE(rows.count(QStringLiteral("Coincident")), 2);
    QCOMPARE(rows.size(), 3);

    // Each row is on screen, with a button that removes its constraint.
    const QList<QToolButton*> removers =
        panel->findChildren<QToolButton*>(QStringLiteral("removeConstraint"));
    int shown = 0;
    for (const QToolButton* remover : removers) {
        shown += remover->isVisible() ? 1 : 0;
    }
    QCOMPARE(shown, 3);

    // Take the Horizontal away with its button: the side can tilt again.
    const int before = static_cast<int>(geometry().constraints().size());
    panel->removeConstraint(static_cast<int>(rows.indexOf(QStringLiteral("Horizontal"))));
    QCOMPARE(static_cast<int>(geometry().constraints().size()), before - 1);
    QCOMPARE(panel->stateText(), QStringLiteral("5 degrees of freedom"));
    QVERIFY(action(window, "edit.undo")->text().contains(QStringLiteral("Delete")));
    QVERIFY(trigger(window, "edit.undo"));
    QCOMPARE(panel->stateText(), QStringLiteral("4 degrees of freedom"));

    // A line selected says what D does with it, and D, pressed for real, does it: the line's
    // length opens for typing in the view.
    editor(window)->setSelection({lines(geometry())[0].id});
    QCOMPARE(panel->promptText(), QStringLiteral("Press D to give it a length"));
    window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    window.viewport()->setFocus();
    QTest::keyClick(&window, Qt::Key_D);
    QVERIFY(editor(window)->valueField() != nullptr);
    QCOMPARE(editor(window)->tool(), SketchTool::Select);
    // A dimension is listed with its value.
    QTest::keyClicks(editor(window)->valueField(), QStringLiteral("64"));
    QTest::keyClick(editor(window)->valueField(), Qt::Key_Return);
    editor(window)->setSelection({lines(geometry())[0].id});
    QVERIFY2(panel->constraintRows().contains(QStringLiteral("Distance 64 mm")),
             qPrintable(panel->constraintRows().join(QStringLiteral(", "))));
    // With nothing selected, Dimension becomes the tool, and the panel says what it waits for.
    editor(window)->clearSelection();
    QVERIFY(panel->promptText().isEmpty());
    QVERIFY(trigger(window, "sketch.dimension"));
    QCOMPARE(editor(window)->tool(), SketchTool::Dimension);
    QVERIFY2(panel->promptText().startsWith(QStringLiteral("Dimension: click a line")),
             qPrintable(panel->promptText()));
    keepFrame(window.grab().toImage(), "sketch-window-dimension-tool");
    editor(window)->cancel();
    QVERIFY(panel->promptText().isEmpty());

    // The tool options are the editor's.
    QVERIFY(panel->inferenceBox()->isChecked());
    panel->inferenceBox()->click();
    QVERIFY(!editor(window)->hasInference());
    QVERIFY(!action(window, "sketch.inference")->isChecked());
    panel->constructionBox()->click();
    QVERIFY(editor(window)->constructionMode());
    QVERIFY(action(window, "sketch.construction")->isChecked());
    panel->sidesField()->setValue(8);
    QCOMPARE(editor(window)->polygonSides(), 8);
    editor(window)->setPolygonSides(5);
    QCOMPARE(panel->sidesField()->value(), 5);

    // A conflict that arrives by other means (a file, a later phase) is said plainly.
    lcad::sketch::Sketch broken = geometry();
    const std::vector<Entity> sides = lines(broken);
    broken.addConstraint(ConstraintKind::Distance, sides[2].start, sides[2].end, 0, 99.0);
    window.document()->setSketchGeometry(id, broken, "Conflict");
    QVERIFY2(panel->stateText().startsWith(QStringLiteral("Over-constrained: 1 constraint")),
             qPrintable(panel->stateText()));
    QVERIFY(window.selectionSummary().contains(QStringLiteral("Over-constrained")));
}

void SketchInterfaceTest::theBrowserListsSketches() {
    MainWindow window;
    show(window);
    QTreeWidget* tree = window.browser()->tree();
    QTreeWidgetItem* root = tree->topLevelItem(0);
    QTreeWidgetItem* group = root->child(2);
    QCOMPARE(group->text(0), QStringLiteral("Sketches"));
    QVERIFY(group->isHidden());

    const ObjectId first = window.createSketch(lcad::kernel::planeXY());
    window.finishSketch();
    const ObjectId second = window.createSketch(lcad::kernel::planeXZ());
    window.finishSketch();
    QVERIFY(!group->isHidden());
    QCOMPARE(group->childCount(), 2);
    QCOMPARE(group->child(0)->text(0), QStringLiteral("Sketch 1"));
    QCOMPARE(group->child(1)->text(0), QStringLiteral("Sketch 2"));
    QCOMPARE(group->child(1), window.browser()->sketchItem(second));

    // Selecting one in the browser is selecting it: the status bar and the panel are about it.
    // (The sketch that was open last is still selected; pick the other.)
    tree->clearSelection();
    window.browser()->sketchItem(first)->setSelected(true);
    QCOMPARE(window.browser()->selectedSketches(), (std::vector<ObjectId>{first}));
    QCOMPARE(window.selectionSummary(), QStringLiteral("Sketch 1 · Nothing drawn yet"));
    QCOMPARE(window.properties()->heading(), QStringLiteral("Sketch 1"));
    QVERIFY(action(window, "sketch.edit")->isEnabled());

    // Its menu offers what applies to a sketch.
    QStringList offered;
    lcad::test::chooseFromNextMenu(QStringLiteral("Edit Sketch"), &offered);
    emit window.browser()->contextMenuRequested(tree->viewport()->mapToGlobal(
        tree->visualItemRect(window.browser()->sketchItem(first)).center()));
    QTRY_VERIFY(window.isSketching());
    QCOMPARE(window.viewport()->editedSketch(), first);
    QVERIFY(offered.contains(QStringLiteral("Edit Sketch")));
    QVERIFY(offered.contains(QStringLiteral("Rename")));
    QVERIFY(offered.contains(QStringLiteral("Hide")));
    QVERIFY(offered.contains(QStringLiteral("Delete")));
    QVERIFY(!offered.contains(QStringLiteral("Isolate")));
    window.finishSketch();

    // The filter narrows sketches as it narrows bodies.
    window.browser()->setFilter(QStringLiteral("2"));
    QVERIFY(window.browser()->sketchItem(first)->isHidden());
    QVERIFY(!window.browser()->sketchItem(second)->isHidden());
    window.browser()->setFilter(QString());
    QVERIFY(!window.browser()->sketchItem(first)->isHidden());

    // Undoing the second sketch's creation takes its entry away; redoing brings it back.
    QVERIFY(trigger(window, "edit.undo"));
    QCOMPARE(group->childCount(), 1);
    QVERIFY(trigger(window, "edit.redo"));
    QCOMPARE(group->childCount(), 2);
}

void SketchInterfaceTest::sketchesAreHiddenShownRenamedAndDeleted() {
    MainWindow window;
    show(window);
    window.startPrimitive(PrimitiveKind::Box);
    window.acceptCommand();
    const ObjectId box = window.document()->bodies()[0].id;
    const ObjectId id = window.createSketch(lcad::kernel::planeXY());
    window.finishSketch();
    const auto sketch = [&window, id] { return window.document()->findSketch(id); };
    QTreeWidgetItem* entry = window.browser()->sketchItem(id);

    // The check box hides and shows it, undoably.
    QCOMPARE(entry->checkState(0), Qt::Checked);
    entry->setCheckState(0, Qt::Unchecked);
    QVERIFY(!sketch()->visible);
    QVERIFY(trigger(window, "edit.undo"));
    QVERIFY(sketch()->visible);
    QCOMPARE(entry->checkState(0), Qt::Checked);

    // Hide and Show are the same commands as for bodies, and take both at once.
    entry->setSelected(true);
    window.browser()->bodyItem(box)->setSelected(true);
    QVERIFY(action(window, "body.hide")->isEnabled());
    QVERIFY(trigger(window, "body.hide"));
    QVERIFY(!sketch()->visible);
    QVERIFY(!window.document()->findBody(box)->visible);
    QCOMPARE(entry->checkState(0), Qt::Unchecked);
    // Hidden, it is still what is selected.
    QCOMPARE(window.selectionSummary(), QStringLiteral("2 items selected"));
    QVERIFY(trigger(window, "body.show"));
    QVERIFY(sketch()->visible && window.document()->findBody(box)->visible);
    // One step for both.
    QVERIFY(trigger(window, "edit.undo"));
    QVERIFY(!sketch()->visible && !window.document()->findBody(box)->visible);
    QVERIFY(trigger(window, "edit.redo"));

    // Rename in place: by F2, or by typing over the name.
    window.browser()->tree()->clearSelection();
    entry->setSelected(true);
    QVERIFY(action(window, "body.rename")->isEnabled());
    entry->setText(0, QStringLiteral("Outline"));
    QCOMPARE(QString::fromStdString(sketch()->name), QStringLiteral("Outline"));
    // An empty name is refused.
    entry->setText(0, QStringLiteral("  "));
    QCOMPARE(QString::fromStdString(sketch()->name), QStringLiteral("Outline"));
    QCOMPARE(entry->text(0), QStringLiteral("Outline"));

    // Delete removes the sketch, and the body selected with it, as one step.
    window.browser()->setSelection({box}, {id});
    QVERIFY(trigger(window, "edit.delete"));
    QVERIFY(sketch() == nullptr);
    QVERIFY(window.document()->bodies().empty());
    QVERIFY(action(window, "edit.undo")->text().contains(QStringLiteral("Delete 2 Items")));
    QVERIFY(trigger(window, "edit.undo"));
    QVERIFY(sketch() != nullptr);
    QCOMPARE(window.document()->bodies().size(), std::size_t{1});
    window.browser()->setSelection({}, {id});
    QVERIFY(trigger(window, "edit.delete"));
    QVERIFY(action(window, "edit.undo")->text().contains(QStringLiteral("Delete Sketch")));
}

void SketchInterfaceTest::theContextMenuInASketchIsAboutTheSketch() {
    MainWindow window;
    show(window);
    startSketch(window);
    QVERIFY(trigger(window, "sketch.line"));
    click(window, at(window, 10.0, 10.0));
    click(window, at(window, 60.0, 30.0));
    trigger(window, "edit.cancel");
    trigger(window, "edit.cancel");

    // Over nothing: the way out, and the view.
    std::unique_ptr<QMenu> menu(window.createViewportMenu(at(window, 100.0, 70.0)));
    QCOMPARE(entriesOf(menu.get()), (QStringList{QStringLiteral("Select"), QStringLiteral("Fit"),
                                                 QStringLiteral("Finish Sketch")}));

    // Over the line: it becomes the selection, and the menu is about it.
    menu.reset(window.createViewportMenu(at(window, 35.0, 20.0)));
    QCOMPARE(editor(window)->selectedEntities().size(), std::size_t{1});
    const QStringList entries = entriesOf(menu.get());
    QVERIFY(entries.contains(QStringLiteral("Dimension")));
    QVERIFY(entries.contains(QStringLiteral("Construction")));
    QVERIFY(entries.contains(QStringLiteral("Constrain")));
    QVERIFY(entries.contains(QStringLiteral("Delete")));
    QVERIFY(entries.contains(QStringLiteral("Finish Sketch")));
    // Nothing about bodies.
    QVERIFY(!entries.contains(QStringLiteral("Hide")));
    QVERIFY(!entries.contains(QStringLiteral("Isolate")));
}

void SketchInterfaceTest::anotherTabOrClosingLeavesTheSketch() {
    MainWindow window;
    show(window);
    auto* modeling = window.findChild<QToolBar*>(QStringLiteral("modelingToolbar"));
    const ObjectId id = startSketch(window);
    QVERIFY(trigger(window, "sketch.circle"));
    click(window, at(window, 30.0, 30.0));
    click(window, at(window, 50.0, 30.0));
    QVERIFY(window.isSketching());

    // Another document: the sketch is closed, and what was drawn is in its document.
    window.newDocument();
    QVERIFY(!window.isSketching());
    QVERIFY(modeling->isVisible());
    QVERIFY(window.document()->sketches().empty());
    QVERIFY(action(window, "sketch.new")->isEnabled());
    window.setCurrentIndex(0);
    QVERIFY(!window.isSketching());
    QCOMPARE(window.document()->sketches().size(), std::size_t{1});
    QCOMPARE(window.document()->findSketch(id)->geometry.entities().size(), std::size_t{3});
    // It is shown there, and can be opened again.
    QVERIFY(window.editSketch(id));
    QVERIFY(window.isSketching());

    // Closing the window while a sketch is open closes the sketch first, so that the layout
    // remembered is the one for modeling.
    window.document()->markSaved();
    window.setCurrentIndex(1);
    window.setCurrentIndex(0);
    QVERIFY(window.editSketch(id));
    QVERIFY(window.close());
    MainWindow next;
    show(next);
    QVERIFY(next.findChild<QToolBar*>(QStringLiteral("modelingToolbar"))->isVisible());
    QVERIFY(!next.findChild<QToolBar*>(QStringLiteral("sketchToolbar"))->isVisible());
}

void SketchInterfaceTest::theSketchToolbarFitsAndDrawsInBothThemes_data() {
    QTest::addColumn<bool>("dark");
    QTest::addColumn<QSize>("size");
    QTest::newRow("sketch light 1280x800") << false << QSize(1280, 800);
    QTest::newRow("sketch dark 1280x800") << true << QSize(1280, 800);
    QTest::newRow("sketch light 1024x700") << false << QSize(1024, 700);
    QTest::newRow("sketch dark 1600x1000") << true << QSize(1600, 1000);
}

void SketchInterfaceTest::theSketchToolbarFitsAndDrawsInBothThemes() {
    QFETCH(bool, dark);
    QFETCH(QSize, size);
    Theme::instance().setChoice(dark ? ThemeChoice::Dark : ThemeChoice::Light);
    MainWindow window;
    window.resize(size);
    show(window);
    if (lcad::test::fitsOnScreen(window, size)) {
        QTRY_COMPARE(window.size(), size);
    } else {
        // A smaller screen keeps the window smaller, and all this has to hold for it as well.
        size = window.size();
    }
    const ObjectId id = startSketch(window);
    QVERIFY(trigger(window, "sketch.rectangle"));
    click(window, at(window, 0.0, 0.0));
    click(window, at(window, 80.0, 50.0));
    QVERIFY(trigger(window, "sketch.circle"));
    click(window, at(window, 40.0, 25.0));
    click(window, at(window, 55.0, 25.0));
    QVERIFY(trigger(window, "sketch.select"));
    editor(window)->setSelection({lines(window.document()->findSketch(id)->geometry)[0].id});
    QVERIFY(editor(window)->addDimension());
    QTest::keyClick(editor(window)->valueField(), Qt::Key_Return);
    render(window);
    keepFrame(window.grab().toImage(), QTest::currentDataTag());

    // Every tool is on the toolbar, whole, with nothing pushed behind an overflow arrow, and
    // each says what it is when pointed at.
    const auto* toolbar = window.findChild<QToolBar*>(QStringLiteral("sketchToolbar"));
    QVERIFY(toolbar->isVisible());
    const auto* overflow =
        toolbar->findChild<QToolButton*>(QStringLiteral("qt_toolbar_ext_button"));
    QVERIFY(overflow == nullptr || !overflow->isVisible());
    QStringList shown;
    for (const QToolButton* button : toolbar->findChildren<QToolButton*>()) {
        if (!button->isVisible() || button == overflow) {
            continue;
        }
        QVERIFY2(toolbar->rect().contains(button->geometry()), qPrintable(button->text()));
        QVERIFY2(!button->toolTip().isEmpty(), qPrintable(button->text()));
        if (button->defaultAction() != nullptr) {
            QVERIFY2(!button->icon().isNull(), qPrintable(button->text()));
            shown << button->defaultAction()->objectName();
        }
    }
    for (const char* tool :
         {"sketch.select", "sketch.line", "sketch.rectangle", "sketch.circle", "sketch.arc",
          "sketch.point", "sketch.polygon", "sketch.slot", "sketch.ellipse", "sketch.spline",
          "sketch.construction", "sketch.trim", "sketch.extend", "sketch.offset", "sketch.mirror",
          "sketch.project", "sketch.dimension", "sketch.finish"}) {
        QVERIFY2(shown.contains(QLatin1String(tool)), tool);
    }
    // The constraints have a button each where there is room, and otherwise one button that
    // opens the list of them. Either way all twelve are a click or two away.
    auto* folded = toolbar->findChild<QToolButton*>(QStringLiteral("constraintsButton"));
    QVERIFY(folded != nullptr);
    const QStringList constraintIds{
        QStringLiteral("sketch.coincident"),    QStringLiteral("sketch.horizontal"),
        QStringLiteral("sketch.vertical"),      QStringLiteral("sketch.parallel"),
        QStringLiteral("sketch.perpendicular"), QStringLiteral("sketch.tangent"),
        QStringLiteral("sketch.equal"),         QStringLiteral("sketch.concentric"),
        QStringLiteral("sketch.midpoint"),      QStringLiteral("sketch.symmetric"),
        QStringLiteral("sketch.collinear"),     QStringLiteral("sketch.fix")};
    if (folded->isVisible()) {
        // Only in a window no wider than the common laptop's 1280.
        QVERIFY(size.width() <= 1280);
        QVERIFY(folded->menu() != nullptr);
        for (const QString& constraint : constraintIds) {
            QVERIFY2(!shown.contains(constraint), qPrintable(constraint));
            QVERIFY2(folded->menu()->actions().contains(window.commands()->action(constraint)),
                     qPrintable(constraint));
        }
    } else {
        for (const QString& constraint : constraintIds) {
            QVERIFY2(shown.contains(constraint), qPrintable(constraint));
        }
    }

    // The colors a sketch is drawn in are the theme's, and can be told apart from the
    // background and from each other.
    const lcad::viewport::ViewportColors colors = window.viewport()->colors();
    QCOMPARE(colors, Theme::instance().tokens().viewport);
    const auto differs = [](const QColor& a, const QColor& b) {
        return qAbs(a.red() - b.red()) + qAbs(a.green() - b.green()) + qAbs(a.blue() - b.blue()) >
               90;
    };
    for (const QColor& ink : {colors.sketchFree, colors.sketchConstrained, colors.sketchReference,
                              colors.sketchConflict, colors.sketch}) {
        QVERIFY(differs(ink, colors.background));
    }
    QVERIFY(differs(colors.sketchFree, colors.sketchConstrained));
    QVERIFY(differs(colors.sketchReference, colors.sketchConstrained));
    QVERIFY(differs(colors.sketchConflict, colors.sketchFree));
    window.finishSketch();
    window.document()->markSaved();
}

QTEST_MAIN(SketchInterfaceTest)
#include "SketchInterfaceTest.moc"
