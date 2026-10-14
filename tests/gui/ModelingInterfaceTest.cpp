// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/BrowserPanel.h>
#include <lcad/app/Commands.h>
#include <lcad/app/FeaturePanel.h>
#include <lcad/app/MainWindow.h>
#include <lcad/app/Theme.h>
#include <lcad/app/TimelinePanel.h>
#include <lcad/document/File.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/Plane.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/sketch/Edit.h>
#include <lcad/viewport/SketchEditor.h>
#include <lcad/viewport/ViewportWidget.h>

#include <FrameChecks.h>
#include <GraphicsMessages.h>

#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>

#include <cmath>

using lcad::app::FeaturePanel;
using lcad::app::MainWindow;
using lcad::app::Theme;
using lcad::app::ThemeChoice;
using lcad::app::TimelinePanel;
using lcad::document::Feature;
using lcad::document::FeatureKind;
using lcad::document::ObjectId;
using lcad::document::Operation;
using lcad::sketch::ConstraintId;
using lcad::sketch::ConstraintKind;
using lcad::test::keepFrame;
using lcad::viewport::SelectedItem;
using lcad::viewport::StandardView;

namespace {

const double pi = 3.14159265358979323846;

bool near(double a, double b, double tolerance = 1e-6) {
    return std::abs(a - b) <= tolerance * std::max(1.0, std::abs(b));
}

// What the bracket's volume should be: a plate `w` by `h`, 8 thick, with its four upright
// corners rounded to 5 and a hole of radius 6 through it.
double bracketVolume(double w, double h) {
    return (w * h - (4.0 - pi) * 25.0 - pi * 36.0) * 8.0;
}

} // namespace

// Making solids as the whole window offers it: Pull, Fillet, and the others started from their
// commands, with what they work on picked in the 3D view and their values typed in their
// panel; the timeline that lists them; and the model following a change to the sketch it was
// built on. The model is checked in the document, the window for showing it.
class ModelingInterfaceTest : public QObject {
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

    void pullMakesASolidFromTheSketchJustDrawn();
    void profilesArePickedByClickingThem();
    void aBracketFollowsTheSketchItWasBuiltOn();
    void theTimelineEditsSuppressesRollsBackAndReorders();
    void aFailingFeatureSaysWhy();
    void edgesArePickedInTheViewForAFilletOrChamfer();
    void shellAndRevolveWork();
    void featuresAreSavedAndOpenedAgain();

private:
    static QAction* action(const MainWindow& window, const char* id) {
        return window.commands()->action(QString::fromLatin1(id));
    }
    static bool trigger(const MainWindow& window, const char* id) {
        return window.commands()->trigger(QString::fromLatin1(id));
    }
    static void show(MainWindow& window) {
        window.resize(1400, 900);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        // Typing goes where the focus is, which needs the window to be the active one.
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
    }
    static void render(const MainWindow& window) { window.viewport()->grabFramebuffer(); }
    static void click(const MainWindow& window, const QPoint& point,
                      Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QTest::mouseMove(window.viewport(), point);
        render(window);
        QTest::mouseClick(window.viewport(), Qt::LeftButton, modifiers, point);
        render(window);
    }
    static QPoint at(const MainWindow& window, double x, double y, double z) {
        return window.viewport()->project(x, y, z).toPoint();
    }
    // A sketch on the XY plane with a rectangle `w` by `h` from the origin, fully dimensioned,
    // drawn while the sketch is open in the window. Returns the sketch and its width dimension.
    static std::pair<ObjectId, ConstraintId> drawPlate(MainWindow& window, double w, double h) {
        const ObjectId id = window.createSketch(lcad::kernel::planeXY());
        lcad::sketch::Sketch s = window.document()->findSketch(id)->geometry;
        const lcad::sketch::RectangleParts r = lcad::sketch::addRectangle(s, {0.0, 0.0}, {w, h});
        s.addConstraint(ConstraintKind::Coincident, s.find(r.bottom)->start, s.origin());
        const ConstraintId width = s.addConstraint(
            ConstraintKind::Distance, s.find(r.bottom)->start, s.find(r.bottom)->end, 0, w);
        s.addConstraint(ConstraintKind::Distance, s.find(r.right)->start, s.find(r.right)->end, 0,
                        h);
        s.solve();
        window.document()->setSketchGeometry(id, s, "Rectangle");
        render(window);
        return {id, width};
    }
    // A sketch on the XY plane with one circle, made without opening it.
    static ObjectId drawHole(MainWindow& window, double x, double y, double radius) {
        const ObjectId id = window.document()->addSketch("Holes", lcad::kernel::planeXY());
        lcad::sketch::Sketch s = window.document()->findSketch(id)->geometry;
        s.addCircle({x, y}, radius);
        window.document()->setSketchGeometry(id, s, "Circle");
        return id;
    }
    static FeaturePanel* panel(const MainWindow& window) { return window.featurePanel(); }
    static void typeInto(FeaturePanel* panel, const char* field, double value) {
        auto* box = panel->findChild<QDoubleSpinBox*>(QString::fromLatin1(field));
        QVERIFY2(box != nullptr, field);
        QVERIFY2(box->isVisible(), field);
        // Tab takes the value and moves on. Enter would take it and accept the command.
        box->setFocus();
        box->selectAll();
        QTest::keyClicks(box, QLocale().toString(value));
        QTest::keyClick(box, Qt::Key_Tab);
        QCOMPARE(box->value(), value);
    }
    static QPushButton* okButton(FeaturePanel* panel) {
        for (QPushButton* button : panel->findChildren<QPushButton*>()) {
            if (button->text().remove(QLatin1Char('&')) == QStringLiteral("OK")) {
                return button;
            }
        }
        return nullptr;
    }
    static const lcad::document::Body& bodyOf(const MainWindow& window, ObjectId feature) {
        return *window.document()->findBody(window.document()->findFeature(feature)->body);
    }
    static double volumeOf(const MainWindow& window, ObjectId feature) {
        return lcad::kernel::volume(bodyOf(window, feature).shape);
    }
    // The four upright edges of a pulled plate, as the 3D view would select them.
    static std::vector<SelectedItem> uprightEdges(const MainWindow& window, ObjectId body) {
        std::vector<SelectedItem> items;
        const std::vector<std::string> names =
            lcad::kernel::edgeNames(window.document()->namedBody(body));
        for (std::size_t i = 0; i < names.size(); ++i) {
            const std::string& name = names[i];
            const std::size_t bar = name.find('|');
            if (bar != std::string::npos && name.find("/side/") < bar &&
                name.find("/side/", bar) != std::string::npos) {
                items.push_back({body, SelectedItem::Kind::Edge, static_cast<int>(i)});
            }
        }
        return items;
    }
    // Pulls the plate's sketch 8 up through the window, and returns the feature.
    static ObjectId pullPlate(MainWindow& window) {
        if (!trigger(window, "solid.pull")) {
            return {};
        }
        FeaturePanel* pull = panel(window);
        if (pull == nullptr) {
            return {};
        }
        typeInto(pull, "distance", 8.0);
        QTest::mouseClick(okButton(pull), Qt::LeftButton);
        return window.document()->features().empty() ? ObjectId{}
                                                     : window.document()->features().back().id;
    }
    static void clickTimeline(const MainWindow& window, ObjectId item, bool twice = false) {
        TimelinePanel* timeline = window.timeline();
        const QRect rect = timeline->itemRect(item);
        QVERIFY(!rect.isEmpty());
        QWidget* target = timeline->childAt(rect.center());
        QVERIFY(target != nullptr);
        const QPoint where = target->mapFrom(timeline, rect.center());
        QTest::mouseClick(target, Qt::LeftButton, Qt::NoModifier, where);
        if (twice) {
            QTest::mouseDClick(target, Qt::LeftButton, Qt::NoModifier, where);
        }
    }

    QTemporaryDir m_directory;
    lcad::test::GraphicsMessages m_graphicsMessages;
};

void ModelingInterfaceTest::init() {
    QTest::failOnWarning(QRegularExpression(QStringLiteral("OpenGL|QRhi|context|surface"),
                                            QRegularExpression::CaseInsensitiveOption));
    QSettings().clear();
    Theme::instance().load();
    QVERIFY(m_directory.isValid());
}

void ModelingInterfaceTest::cleanup() {
    const QStringList complaints = m_graphicsMessages.take();
    QVERIFY2(complaints.isEmpty(), qPrintable(complaints.join(QStringLiteral(" | "))));
}

void ModelingInterfaceTest::pullMakesASolidFromTheSketchJustDrawn() {
    MainWindow window;
    show(window);
    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("propertiesDock"));
    // Without a sketch there is nothing to pull, and without a body nothing to round.
    QVERIFY(!action(window, "solid.pull")->isEnabled());
    QVERIFY(!action(window, "solid.fillet")->isEnabled());
    QCOMPARE(action(window, "solid.pull")->shortcut(), QKeySequence(Qt::Key_E));
    QCOMPARE(action(window, "solid.pull")->text().remove(QLatin1Char('&')), QStringLiteral("Pull"));

    const auto [sketch, width] = drawPlate(window, 60.0, 40.0);
    QVERIFY(window.isSketching());
    // 8. Pull, straight from the sketch: the sketch is finished, and its one closed area is
    // picked already.
    QVERIFY(action(window, "solid.pull")->isEnabled());
    QVERIFY(trigger(window, "solid.pull"));
    QVERIFY(!window.isSketching());
    FeaturePanel* pull = panel(window);
    QVERIFY(pull != nullptr);
    QVERIFY(pull->isVisible());
    QCOMPARE(dock->windowTitle(), QStringLiteral("Pull"));
    QCOMPARE(window.commandText(), QStringLiteral("Pull"));
    // Shown whole in the status bar.
    const auto* command = window.findChild<QLabel*>(QStringLiteral("activeCommand"));
    QTRY_VERIFY(command->width() >= command->sizeHint().width());
    QVERIFY(window.viewport()->isProfilePicking());
    QCOMPARE(window.viewport()->pickedProfiles().size(), std::size_t{1});
    QCOMPARE(pull->feature().sketch, sketch);
    QCOMPARE(pull->feature().regions.size(), std::size_t{1});
    QVERIFY(pull->findChild<QLabel*>(QStringLiteral("featureInput"))
                ->text()
                .contains(QStringLiteral("1 area")));
    // With no body to join, it makes one.
    QCOMPARE(pull->feature().operation, Operation::NewBody);
    QVERIFY(okButton(pull)->isEnabled());
    // Nothing has changed yet: what is shown is a preview.
    QVERIFY(window.document()->features().empty());
    QVERIFY(window.document()->bodies().empty());
    render(window);

    // An exact distance, typed. A distance of zero is refused with the reason.
    typeInto(pull, "distance", 0.0);
    QVERIFY(!okButton(pull)->isEnabled());
    QVERIFY(pull->errorText().contains(QStringLiteral("zero")));
    typeInto(pull, "distance", 8.0);
    QVERIFY(pull->errorText().isEmpty());
    QVERIFY(okButton(pull)->isEnabled());
    render(window);
    keepFrame(window.grab().toImage(), "modeling-pull-preview");
    QTest::mouseClick(okButton(pull), Qt::LeftButton);

    QTRY_VERIFY(panel(window) == nullptr);
    QVERIFY(!window.viewport()->isProfilePicking());
    QCOMPARE(dock->windowTitle(), QStringLiteral("Properties"));
    QCOMPARE(window.document()->features().size(), std::size_t{1});
    const Feature& made = window.document()->features().front();
    QCOMPARE(QString::fromStdString(made.name), QStringLiteral("Pull 1"));
    QVERIFY(window.document()->featureError(made.id).empty());
    QVERIFY(near(volumeOf(window, made.id), 60.0 * 40.0 * 8.0));
    // The body is listed, and the sketch has done its work and is hidden.
    QCOMPARE(window.document()->bodies().size(), std::size_t{1});
    QVERIFY(window.browser()->bodyItem(made.body) != nullptr);
    QVERIFY(!window.document()->findSketch(sketch)->visible);
    // One step to undo.
    QCOMPARE(QString::fromStdString(window.document()->undoLabel()), QStringLiteral("Pull"));
    // The timeline lists the sketch and the pull, in that order, and selects the new one.
    QCOMPARE(window.document()->timeline(), (std::vector<ObjectId>{sketch, made.id}));
    QCOMPARE(window.timeline()->selectedItem(), made.id);
    QVERIFY(window.timeline()->itemRect(sketch).right() <
            window.timeline()->itemRect(made.id).left());
    QVERIFY(window.timeline()->describe(made.id).contains(QStringLiteral("Made from Sketch 1")));
    render(window);
    keepFrame(window.grab().toImage(), "modeling-pulled");

    QVERIFY(trigger(window, "edit.undo"));
    QVERIFY(window.document()->features().empty());
    QVERIFY(window.document()->bodies().empty());
    QVERIFY(window.document()->findSketch(sketch)->visible);
    QVERIFY(trigger(window, "edit.redo"));
    QVERIFY(near(volumeOf(window, made.id), 60.0 * 40.0 * 8.0));

    // Esc gives up a pull without a trace.
    QVERIFY(trigger(window, "solid.pull"));
    QVERIFY(panel(window) != nullptr);
    QVERIFY(trigger(window, "edit.cancel"));
    QTRY_VERIFY(panel(window) == nullptr);
    QCOMPARE(window.document()->features().size(), std::size_t{1});
    QCOMPARE(QString::fromStdString(window.document()->undoLabel()), QStringLiteral("Pull"));
}

void ModelingInterfaceTest::profilesArePickedByClickingThem() {
    MainWindow window;
    show(window);
    // A plate outline with a circle inside it: two closed areas, the plate with a hole, and the
    // disc in the hole.
    const auto [sketch, width] = drawPlate(window, 60.0, 40.0);
    lcad::sketch::Sketch s = window.document()->findSketch(sketch)->geometry;
    s.addCircle({20.0, 20.0}, 6.0);
    window.document()->setSketchGeometry(sketch, s, "Circle");
    window.finishSketch();
    window.viewport()->setStandardView(StandardView::Top);
    render(window);

    // Not open, and not selected: nothing is picked to start with, as there is a choice.
    QVERIFY(trigger(window, "solid.pull"));
    FeaturePanel* pull = panel(window);
    QVERIFY(pull != nullptr);
    QCOMPARE(window.viewport()->pickableProfiles().size(), std::size_t{2});
    QVERIFY(window.viewport()->pickedProfiles().empty());
    QVERIFY(!okButton(pull)->isEnabled());
    QVERIFY(pull->findChild<QLabel*>(QStringLiteral("featureInput"))
                ->text()
                .contains(QStringLiteral("Click")));

    // A click on the plate picks the plate, and one on the disc adds the disc.
    const QPoint onPlate = at(window, 45.0, 30.0, 0.0);
    const QPoint onDisc = at(window, 20.0, 20.0, 0.0);
    QVERIFY(window.viewport()->profileAt(onPlate).has_value());
    QVERIFY(window.viewport()->profileAt(onPlate)->outline.size() == 4);
    QVERIFY(window.viewport()->profileAt(onDisc)->outline.size() == 1);
    click(window, onPlate);
    QCOMPARE(window.viewport()->pickedProfiles().size(), std::size_t{1});
    QCOMPARE(pull->feature().regions.size(), std::size_t{1});
    QCOMPARE(pull->feature().regions.front().size(), std::size_t{4});
    QVERIFY(okButton(pull)->isEnabled());
    // Clicking picks; it does not select.
    QVERIFY(window.viewport()->selection().empty());
    click(window, onDisc);
    QCOMPARE(pull->feature().regions.size(), std::size_t{2});
    render(window);
    keepFrame(window.grab().toImage(), "modeling-profiles-picked");
    // And a second click lets it go again.
    click(window, onDisc);
    QCOMPARE(pull->feature().regions.size(), std::size_t{1});
    // A click beside the sketch does nothing.
    click(window, at(window, 90.0, 20.0, 0.0));
    QCOMPARE(pull->feature().regions.size(), std::size_t{1});

    typeInto(pull, "distance", 5.0);
    QTest::keyClick(pull, Qt::Key_Return);
    QTRY_VERIFY(panel(window) == nullptr);
    const Feature& plate = window.document()->features().back();
    QVERIFY(near(volumeOf(window, plate.id), (60.0 * 40.0 - pi * 36.0) * 5.0));
}

void ModelingInterfaceTest::aBracketFollowsTheSketchItWasBuiltOn() {
    MainWindow window;
    show(window);
    const auto [outline, width] = drawPlate(window, 60.0, 40.0);
    const ObjectId plate = pullPlate(window);
    QVERIFY(plate.isValid());
    QTRY_VERIFY(panel(window) == nullptr);
    const ObjectId body = bodyOf(window, plate).id;

    // A hole: a circle in a sketch of its own, pulled through the plate. Starting inside the
    // body, the pull is taken to be a cut.
    const ObjectId holes = drawHole(window, 20.0, 20.0, 6.0);
    window.browser()->setSelection({}, {holes});
    QVERIFY(trigger(window, "solid.pull"));
    FeaturePanel* pull = panel(window);
    QVERIFY(pull != nullptr);
    QCOMPARE(pull->feature().regions.size(), std::size_t{1});
    typeInto(pull, "distance", 8.0);
    QCOMPARE(pull->feature().operation, Operation::Cut);
    QCOMPARE(pull->feature().body, body);
    QVERIFY(okButton(pull)->isEnabled());
    QTest::mouseClick(okButton(pull), Qt::LeftButton);
    QTRY_VERIFY(panel(window) == nullptr);
    const ObjectId hole = window.document()->features().back().id;
    QVERIFY(window.document()->featureError(hole).empty());
    QVERIFY(near(volumeOf(window, plate), (60.0 * 40.0 - pi * 36.0) * 8.0));

    // 9. Fillet the four upright edges, picked before the command.
    QVERIFY(action(window, "solid.fillet")->isEnabled());
    window.viewport()->setSelectionFilter(lcad::viewport::SelectionFilter::Edges);
    window.viewport()->setSelection(uprightEdges(window, body));
    QCOMPARE(window.viewport()->selection().size(), std::size_t{4});
    QVERIFY(trigger(window, "solid.fillet"));
    FeaturePanel* fillet = panel(window);
    QVERIFY(fillet != nullptr);
    QCOMPARE(fillet->feature().edges.size(), std::size_t{4});
    QCOMPARE(fillet->feature().body, body);
    typeInto(fillet, "size", 5.0);
    QVERIFY(fillet->errorText().isEmpty());
    render(window);
    keepFrame(window.grab().toImage(), "modeling-fillet-preview");
    // A radius larger than the plate allows is refused with the kernel's reason.
    typeInto(fillet, "size", 30.0);
    QVERIFY(!fillet->errorText().isEmpty());
    QVERIFY(!okButton(fillet)->isEnabled());
    typeInto(fillet, "size", 5.0);
    QTest::mouseClick(okButton(fillet), Qt::LeftButton);
    QTRY_VERIFY(panel(window) == nullptr);
    const ObjectId rounds = window.document()->features().back().id;
    QVERIFY(near(volumeOf(window, plate), bracketVolume(60.0, 40.0)));
    // The selection filter is what it was before the fillet.
    QCOMPARE(window.viewport()->selectionFilter(), lcad::viewport::SelectionFilter::Edges);
    render(window);
    keepFrame(window.grab().toImage(), "modeling-bracket");

    // 10.-11. Edit the original sketch, from the browser, though it is hidden; change the
    // width; finish. The hole and the rounds follow.
    QTreeWidget* tree = window.browser()->tree();
    QTreeWidgetItem* entry = window.browser()->sketchItem(outline);
    QVERIFY(entry != nullptr);
    tree->scrollToItem(entry);
    const QPoint onEntry = tree->visualItemRect(entry).center();
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, onEntry);
    QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, onEntry);
    QVERIFY(window.isSketching());
    render(window);
    auto* editor = window.viewport()->sketchEditor();
    const QPoint onValue = editor->dimensionRect(width).center().toPoint();
    QTest::mouseMove(window.viewport(), onValue);
    QTest::mouseDClick(window.viewport(), Qt::LeftButton, Qt::NoModifier, onValue);
    render(window);
    QLineEdit* field = editor->valueField();
    QVERIFY(field != nullptr);
    QTest::keyClicks(field, QStringLiteral("85"));
    QTest::keyClick(field, Qt::Key_Return);
    // While the sketch is open, the model waits for it.
    QVERIFY(near(volumeOf(window, plate), bracketVolume(60.0, 40.0)));
    QVERIFY(trigger(window, "sketch.finish"));
    QVERIFY(!window.isSketching());
    // 12. The model has followed.
    QVERIFY(near(volumeOf(window, plate), bracketVolume(85.0, 40.0)));
    for (const ObjectId id : {plate, hole, rounds}) {
        QVERIFY(window.document()->featureError(id).empty());
    }
    render(window);
    keepFrame(window.grab().toImage(), "modeling-bracket-wider");

    // 13. Select a feature in the history: its body is selected, and the timeline says what it
    // is about.
    clickTimeline(window, rounds);
    QCOMPARE(window.timeline()->selectedItem(), rounds);
    QCOMPARE(window.viewport()->selection(),
             (std::vector<SelectedItem>{{body, SelectedItem::Kind::Body, -1}}));
    QVERIFY(window.timeline()->describe(rounds).contains(QStringLiteral("Works on Body")));

    // Double-click the first pull to edit it: the model goes back to before it, and its panel
    // has its values.
    clickTimeline(window, plate, true);
    FeaturePanel* edit = panel(window);
    QVERIFY(edit != nullptr);
    QCOMPARE(window.editedFeature(), plate);
    QCOMPARE(edit->value(QStringLiteral("distance")), 8.0);
    QCOMPARE(window.findChild<QDockWidget*>(QStringLiteral("propertiesDock"))->windowTitle(),
             QStringLiteral("Edit Pull 1"));
    QVERIFY(window.document()->findBody(body)->shape.isNull());
    QCOMPARE(window.viewport()->pickedProfiles().size(), std::size_t{1});
    typeInto(edit, "distance", 10.0);
    QTest::mouseClick(okButton(edit), Qt::LeftButton);
    QTRY_VERIFY(panel(window) == nullptr);
    QVERIFY(!window.editedFeature().isValid());
    // The hole was pulled 8 deep, which no longer reaches through a plate 10 thick.
    const double thicker = (85.0 * 40.0 - (4.0 - pi) * 25.0) * 10.0 - pi * 36.0 * 8.0;
    QVERIFY(near(volumeOf(window, plate), thicker));
    QCOMPARE(QString::fromStdString(window.document()->undoLabel()), QStringLiteral("Edit Pull 1"));
    QVERIFY(trigger(window, "edit.undo"));
    QVERIFY(near(volumeOf(window, plate), bracketVolume(85.0, 40.0)));
}

void ModelingInterfaceTest::theTimelineEditsSuppressesRollsBackAndReorders() {
    MainWindow window;
    show(window);
    const auto [outline, width] = drawPlate(window, 60.0, 40.0);
    const ObjectId plate = pullPlate(window);
    QTRY_VERIFY(panel(window) == nullptr);
    const ObjectId body = bodyOf(window, plate).id;
    window.viewport()->setSelectionFilter(lcad::viewport::SelectionFilter::Edges);
    window.viewport()->setSelection(uprightEdges(window, body));
    QVERIFY(trigger(window, "solid.fillet"));
    typeInto(panel(window), "size", 5.0);
    QTest::mouseClick(okButton(panel(window)), Qt::LeftButton);
    QTRY_VERIFY(panel(window) == nullptr);
    const ObjectId rounds = window.document()->features().back().id;
    const double rounded = (60.0 * 40.0 - (4.0 - pi) * 25.0) * 8.0;
    QVERIFY(near(volumeOf(window, plate), rounded));

    // Suppress the fillet from the timeline: the corners are square again, and the entry says
    // so.
    clickTimeline(window, rounds);
    QVERIFY(action(window, "timeline.suppress")->isEnabled());
    QCOMPARE(action(window, "timeline.suppress")->text().remove(QLatin1Char('&')),
             QStringLiteral("Suppress Feature"));
    QVERIFY(trigger(window, "timeline.suppress"));
    QVERIFY(near(volumeOf(window, plate), 60.0 * 40.0 * 8.0));
    QVERIFY(window.timeline()->describe(rounds).contains(QStringLiteral("Suppressed")));

    QCOMPARE(action(window, "timeline.suppress")->text().remove(QLatin1Char('&')),
             QStringLiteral("Restore Feature"));
    QVERIFY(trigger(window, "timeline.suppress"));
    QVERIFY(near(volumeOf(window, plate), rounded));

    // Drag the history marker back past the fillet: the model is as it was before it.
    TimelinePanel* timeline = window.timeline();
    QCOMPARE(window.document()->historyMarker(), std::size_t{3});
    auto* strip = timeline->findChild<QWidget*>(QStringLiteral("timeline"));
    QVERIFY(strip != nullptr);
    const QPoint from = strip->mapFrom(timeline, timeline->markerRect().center());
    const QPoint to = strip->mapFrom(timeline, QPoint(timeline->itemRect(rounds).left() - 2,
                                                      timeline->itemRect(rounds).center().y()));
    QTest::mousePress(strip, Qt::LeftButton, Qt::NoModifier, from);
    QTest::mouseMove(strip, (from + to) / 2);
    QTest::mouseMove(strip, to);
    QTest::mouseRelease(strip, Qt::LeftButton, Qt::NoModifier, to);
    QCOMPARE(window.document()->historyMarker(), std::size_t{2});
    QVERIFY(near(volumeOf(window, plate), 60.0 * 40.0 * 8.0));
    QVERIFY(timeline->describe(rounds).contains(QStringLiteral("After the history marker")));
    render(window);
    keepFrame(window.grab().toImage(), "modeling-rolled-back");
    QVERIFY(action(window, "timeline.rollToEnd")->isEnabled());
    QVERIFY(trigger(window, "timeline.rollToEnd"));
    QCOMPARE(window.document()->historyMarker(), std::size_t{3});
    QVERIFY(near(volumeOf(window, plate), rounded));
    // Roll Back to Here does it from a menu.
    clickTimeline(window, plate);
    QVERIFY(trigger(window, "timeline.rollBack"));
    QCOMPARE(window.document()->historyMarker(), std::size_t{1});
    QVERIFY(window.document()->findBody(body)->shape.isNull());
    QVERIFY(trigger(window, "edit.undo"));
    QCOMPARE(window.document()->historyMarker(), std::size_t{3});

    // Dragging the fillet in front of the pull that makes its body is refused, with the reason.
    const QPoint grab = strip->mapFrom(timeline, timeline->itemRect(rounds).center());
    const QPoint drop = strip->mapFrom(timeline, QPoint(timeline->itemRect(plate).left() - 2,
                                                        timeline->itemRect(plate).center().y()));
    QTest::mousePress(strip, Qt::LeftButton, Qt::NoModifier, grab);
    QTest::mouseMove(strip, (grab + drop) / 2);
    QTest::mouseMove(strip, drop);
    QTest::mouseRelease(strip, Qt::LeftButton, Qt::NoModifier, drop);
    QCOMPARE(window.document()->timeline(), (std::vector<ObjectId>{outline, plate, rounds}));
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("cannot come before")));

    // A sketch that nothing depends on can be moved, by dragging it.
    const ObjectId spare = window.document()->addSketch("Spare", lcad::kernel::planeXZ());
    QCOMPARE(window.document()->timeline().back(), spare);
    const QPoint spareAt = strip->mapFrom(timeline, timeline->itemRect(spare).center());
    const QPoint first = strip->mapFrom(timeline, QPoint(timeline->itemRect(outline).left() - 2,
                                                         timeline->itemRect(outline).center().y()));
    QTest::mousePress(strip, Qt::LeftButton, Qt::NoModifier, spareAt);
    QTest::mouseMove(strip, (spareAt + first) / 2);
    QTest::mouseMove(strip, first);
    QTest::mouseRelease(strip, Qt::LeftButton, Qt::NoModifier, first);
    QCOMPARE(window.document()->timeline().front(), spare);
    QVERIFY(near(volumeOf(window, plate), rounded));

    // Rename and delete, from the timeline.
    window.document()->renameFeature(rounds, "Corner rounds");
    QVERIFY(timeline->describe(rounds).startsWith(QStringLiteral("Corner rounds · Fillet")));
    clickTimeline(window, rounds);
    window.timeline()->findChild<QWidget*>(QStringLiteral("timeline"))->setFocus();
    QVERIFY(trigger(window, "edit.delete"));
    QVERIFY(window.document()->findFeature(rounds) == nullptr);
    QVERIFY(near(volumeOf(window, plate), 60.0 * 40.0 * 8.0));
    QVERIFY(timeline->itemRect(rounds).isEmpty());
    QVERIFY(!timeline->selectedItem().isValid());

    // Compact makes the entries smaller.
    const int normal = timeline->itemRect(plate).height();
    timeline->findChild<QToolButton*>(QStringLiteral("timelineCompact"))->click();
    QVERIFY(timeline->isCompact());
    QVERIFY(timeline->itemRect(plate).height() < normal);
}

void ModelingInterfaceTest::aFailingFeatureSaysWhy() {
    MainWindow window;
    show(window);
    drawPlate(window, 60.0, 40.0);
    const ObjectId plate = pullPlate(window);
    QTRY_VERIFY(panel(window) == nullptr);
    const ObjectId body = bodyOf(window, plate).id;
    const ObjectId holes = drawHole(window, 20.0, 20.0, 6.0);
    window.browser()->setSelection({}, {holes});
    QVERIFY(trigger(window, "solid.pull"));
    typeInto(panel(window), "distance", 8.0);
    QTest::mouseClick(okButton(panel(window)), Qt::LeftButton);
    QTRY_VERIFY(panel(window) == nullptr);
    const ObjectId hole = window.document()->features().back().id;

    // Move the circle off the plate: the cut no longer reaches it. The model keeps what it can,
    // and the cut says why it fails, in the timeline and when it is selected.
    lcad::sketch::Sketch s = window.document()->findSketch(holes)->geometry;
    for (const lcad::sketch::Entity& entity : s.entities()) {
        if (entity.kind == lcad::sketch::EntityKind::Circle) {
            s.dragBy(entity.id, 180.0, 180.0);
        }
    }
    window.document()->setSketchGeometry(holes, s, "Move Circle");
    QVERIFY(!window.document()->featureError(hole).empty());
    QVERIFY(near(volumeOf(window, plate), 60.0 * 40.0 * 8.0));
    QVERIFY(window.timeline()->describe(hole).contains(QStringLiteral("Fails: It does not reach")));
    clickTimeline(window, hole);
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("Pull 2 fails")));
    render(window);
    keepFrame(window.grab().toImage(), "modeling-failing-feature");

    // Editing it shows the same reason in its panel, and OK waits until it works.
    QVERIFY(trigger(window, "timeline.edit"));
    QVERIFY(panel(window) != nullptr);
    QVERIFY(panel(window)->errorText().contains(QStringLiteral("does not reach")));
    QVERIFY(!okButton(panel(window))->isEnabled());
    QVERIFY(trigger(window, "edit.cancel"));
    QTRY_VERIFY(panel(window) == nullptr);
    // Put right, it works again.
    QVERIFY(trigger(window, "edit.undo"));
    QVERIFY(window.document()->featureError(hole).empty());
    QVERIFY(near(volumeOf(window, plate), (60.0 * 40.0 - pi * 36.0) * 8.0));
    Q_UNUSED(body);
}

void ModelingInterfaceTest::edgesArePickedInTheViewForAFilletOrChamfer() {
    MainWindow window;
    show(window);
    drawPlate(window, 60.0, 40.0);
    const ObjectId plate = pullPlate(window);
    QTRY_VERIFY(panel(window) == nullptr);
    const ObjectId body = bodyOf(window, plate).id;
    window.viewport()->setStandardView(StandardView::Home);
    render(window);

    // Chamfer, with nothing selected: the panel asks for edges, and the 3D view picks edges.
    QVERIFY(trigger(window, "solid.chamfer"));
    FeaturePanel* chamfer = panel(window);
    QVERIFY(chamfer != nullptr);
    QCOMPARE(window.viewport()->selectionFilter(), lcad::viewport::SelectionFilter::Edges);
    QVERIFY(!okButton(chamfer)->isEnabled());
    // The upright edge at the front right corner, (60, 0), halfway up.
    click(window, at(window, 60.0, 0.0, 4.0));
    QCOMPARE(chamfer->feature().edges.size(), std::size_t{1});
    QCOMPARE(chamfer->feature().body, body);
    // Shift-click adds the one at the front left.
    click(window, at(window, 0.0, 0.0, 4.0), Qt::ShiftModifier);
    QCOMPARE(chamfer->feature().edges.size(), std::size_t{2});
    typeInto(chamfer, "size", 2.0);
    QVERIFY(okButton(chamfer)->isEnabled());
    QTest::mouseClick(okButton(chamfer), Qt::LeftButton);
    QTRY_VERIFY(panel(window) == nullptr);
    // Two corners lose a right triangle with legs of 2, all the way up.
    QVERIFY(near(volumeOf(window, plate), (60.0 * 40.0 - 2.0 * 2.0) * 8.0));
    // The filter is back to what it was.
    QCOMPARE(window.viewport()->selectionFilter(),
             lcad::viewport::SelectionFilter::FacesEdgesVertices);
}

void ModelingInterfaceTest::shellAndRevolveWork() {
    MainWindow window;
    show(window);
    drawPlate(window, 60.0, 40.0);
    const ObjectId plate = pullPlate(window);
    QTRY_VERIFY(panel(window) == nullptr);
    const ObjectId body = bodyOf(window, plate).id;

    // Shell, open at the top face: picked in the view.
    window.viewport()->setStandardView(StandardView::Home);
    render(window);
    QVERIFY(trigger(window, "solid.shell"));
    FeaturePanel* shell = panel(window);
    QVERIFY(shell != nullptr);
    QCOMPARE(window.viewport()->selectionFilter(), lcad::viewport::SelectionFilter::Faces);
    click(window, at(window, 30.0, 20.0, 8.0));
    QCOMPARE(shell->feature().faces.size(), std::size_t{1});
    QVERIFY(shell->feature().faces.front().find("/end/") != std::string::npos);
    typeInto(shell, "size", 2.0);
    QVERIFY(shell->errorText().isEmpty());
    QTest::mouseClick(okButton(shell), Qt::LeftButton);
    QTRY_VERIFY(panel(window) == nullptr);
    QVERIFY(near(volumeOf(window, plate), 60.0 * 40.0 * 8.0 - 56.0 * 36.0 * 6.0));

    // Revolve: a rectangle beside a construction line, turned all the way round.
    const ObjectId id = window.createSketch(lcad::kernel::planeXZ());
    lcad::sketch::Sketch s = window.document()->findSketch(id)->geometry;
    lcad::sketch::addRectangle(s, {100.0, 0.0}, {110.0, 20.0});
    const lcad::sketch::EntityId axis = s.addLine({90.0, 0.0}, {90.0, 30.0});
    s.setConstruction(axis, true);
    window.document()->setSketchGeometry(id, s, "Profile");
    QVERIFY(trigger(window, "solid.revolve"));
    FeaturePanel* revolve = panel(window);
    QVERIFY(revolve != nullptr);
    // The construction line is offered first as the axis.
    auto* axes = revolve->findChild<QComboBox*>(QStringLiteral("axis"));
    QVERIFY(axes != nullptr);
    QVERIFY(axes->count() == 5);
    QVERIFY(axes->currentText().contains(QStringLiteral("construction")));
    QCOMPARE(revolve->feature().axis, axis);
    // Apart from the shelled plate, so a body of its own.
    QCOMPARE(revolve->feature().operation, Operation::NewBody);
    QVERIFY(revolve->errorText().isEmpty());
    QTest::mouseClick(okButton(revolve), Qt::LeftButton);
    QTRY_VERIFY(panel(window) == nullptr);
    const ObjectId ring = window.document()->features().back().id;
    // A ring from radius 10 to 20, 20 high.
    QVERIFY(near(volumeOf(window, ring), pi * (400.0 - 100.0) * 20.0, 1e-5));
    QCOMPARE(window.document()->bodies().size(), std::size_t{2});
    Q_UNUSED(body);
}

void ModelingInterfaceTest::featuresAreSavedAndOpenedAgain() {
    const QString path = m_directory.filePath(QStringLiteral("bracket.lcad"));
    double volume = 0.0;
    {
        MainWindow window;
        show(window);
        drawPlate(window, 60.0, 40.0);
        const ObjectId plate = pullPlate(window);
        QTRY_VERIFY(panel(window) == nullptr);
        const ObjectId body = bodyOf(window, plate).id;
        window.viewport()->setSelectionFilter(lcad::viewport::SelectionFilter::Edges);
        window.viewport()->setSelection(uprightEdges(window, body));
        QVERIFY(trigger(window, "solid.fillet"));
        typeInto(panel(window), "size", 5.0);
        QTest::mouseClick(okButton(panel(window)), Qt::LeftButton);
        QTRY_VERIFY(panel(window) == nullptr);
        volume = volumeOf(window, plate);
        QString error;
        QVERIFY2(window.saveTo(path, &error), qPrintable(error));
    }
    MainWindow window;
    show(window);
    QString error;
    QVERIFY2(window.loadFrom(path, &error), qPrintable(error));
    QVERIFY(window.loadWarnings().isEmpty());
    QCOMPARE(window.document()->features().size(), std::size_t{2});
    QVERIFY(near(volumeOf(window, window.document()->features().front().id), volume));
    // The timeline shows what was saved.
    for (const ObjectId id : window.document()->timeline()) {
        QVERIFY(!window.timeline()->itemRect(id).isEmpty());
    }
}

QTEST_MAIN(ModelingInterfaceTest)
#include "ModelingInterfaceTest.moc"
