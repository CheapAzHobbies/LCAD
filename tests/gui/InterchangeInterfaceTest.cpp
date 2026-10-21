// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/BrowserPanel.h>
#include <lcad/app/Commands.h>
#include <lcad/app/ExportDialog.h>
#include <lcad/app/FeaturePanel.h>
#include <lcad/app/GeometryCheckDialog.h>
#include <lcad/app/MainWindow.h>
#include <lcad/app/Theme.h>
#include <lcad/app/TimelinePanel.h>
#include <lcad/kernel/Check.h>
#include <lcad/kernel/Exchange.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/Plane.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/kernel/Transform.h>
#include <lcad/viewport/ViewportWidget.h>

#include <FrameChecks.h>
#include <GraphicsMessages.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

#include <cmath>
#include <functional>
#include <string>

using lcad::app::ExportDialog;
using lcad::app::ExportFormat;
using lcad::app::ExportOptions;
using lcad::app::FeaturePanel;
using lcad::app::GeometryCheckDialog;
using lcad::app::MainWindow;
using lcad::document::FeatureKind;
using lcad::document::ObjectId;
using lcad::test::keepFrame;
using lcad::viewport::SelectedItem;
using lcad::viewport::StandardView;

namespace {

const double pi = 3.14159265358979323846;

bool nearly(double a, double b, double tolerance = 1e-6) {
    return std::abs(a - b) <= tolerance * std::max(1.0, std::abs(b));
}

// What the plate the tests import holds: 60 by 40 by `thick`, with a hole of radius 5.
double plateVolume(double thick) {
    return (60.0 * 40.0 - pi * 25.0) * thick;
}

} // namespace

// Geometry from other programs, as the window offers it: Import, the direct edits that work on
// what was imported, Check Geometry and Repair, and Export. The file dialogs themselves are
// Qt's; what they lead to is driven here through the functions they call.
class InterchangeInterfaceTest : public QObject {
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

    void aStepSolidIsImportedOffsetSavedAndExported();
    void facesAreMovedAndDeleted();
    void checkGeometryFindsProblemsAndRepairFixesThem();
    void theExportDialogShowsOnlyWhatApplies();

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
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
    }
    static void render(const MainWindow& window) { window.viewport()->grabFramebuffer(); }
    static void click(const MainWindow& window, const QPoint& point) {
        QTest::mouseMove(window.viewport(), point);
        render(window);
        QTest::mouseClick(window.viewport(), Qt::LeftButton, Qt::NoModifier, point);
        render(window);
    }
    static QPoint at(const MainWindow& window, double x, double y, double z) {
        return window.viewport()->project(x, y, z).toPoint();
    }
    static void typeInto(FeaturePanel* panel, const char* field, double value) {
        auto* box = panel->findChild<QDoubleSpinBox*>(QString::fromLatin1(field));
        QVERIFY2(box != nullptr, field);
        QVERIFY2(box->isVisible(), field);
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
    static double volumeOf(const MainWindow& window, ObjectId body) {
        return lcad::kernel::volume(window.document()->findBody(body)->shape);
    }
    // The number of the one face of a body that `pick` is true of.
    static int faceWhere(const MainWindow& window, ObjectId body,
                         const std::function<bool(const lcad::kernel::Shape&)>& pick) {
        const lcad::kernel::NamedShape named = window.document()->namedBody(body);
        int found = -1;
        for (int i = 0; i < static_cast<int>(named.faceNames.size()); ++i) {
            if (pick(lcad::kernel::subShape(named.shape, lcad::kernel::SubShapeKind::Face, i))) {
                found = i;
            }
        }
        return found;
    }
    static bool isRound(const lcad::kernel::Shape& face) {
        return lcad::kernel::radiusOf(face).has_value() &&
               !lcad::kernel::planeOfFace(face).has_value();
    }
    // Writes the plate to a STEP file, as another program would hand it over.
    QString stepPlate() const {
        using namespace lcad::kernel;
        const NamedShape box = nameFaces(makeBox(60.0, 40.0, 10.0), "b");
        const NamedShape pin =
            nameFaces(translated(makeCylinder(5.0, 12.0), 30.0, 20.0, -1.0), "p");
        const QString path = m_directory.filePath(QStringLiteral("bracket.step"));
        exportStep({{"Bracket", combine(box, pin, Combination::Cut).shape}},
                   std::filesystem::path(path.toStdString()));
        return path;
    }
    ObjectId importPlate(MainWindow& window) const {
        QString error;
        QStringList notes;
        if (!window.importGeometry(stepPlate(), &error, &notes)) {
            qWarning("%s", qPrintable(error));
            return {};
        }
        return window.document()->bodies().back().id;
    }

    QTemporaryDir m_directory;
    lcad::test::GraphicsMessages m_graphicsMessages;
};

void InterchangeInterfaceTest::init() {
    QTest::failOnWarning(QRegularExpression(QStringLiteral("OpenGL|QRhi|context|surface"),
                                            QRegularExpression::CaseInsensitiveOption));
    QSettings().clear();
    lcad::app::Theme::instance().load();
    QVERIFY(m_directory.isValid());
}

void InterchangeInterfaceTest::cleanup() {
    const QStringList complaints = m_graphicsMessages.take();
    QVERIFY2(complaints.isEmpty(), qPrintable(complaints.join(QStringLiteral(" | "))));
}

void InterchangeInterfaceTest::aStepSolidIsImportedOffsetSavedAndExported() {
    MainWindow window;
    show(window);
    QVERIFY(action(window, "file.import")->isEnabled());
    QVERIFY(!action(window, "file.export")->isEnabled());

    // Import: one body, named as in the file, selected and in view, in one undo step.
    QString error;
    QStringList notes;
    QVERIFY2(window.importGeometry(stepPlate(), &error, &notes), qPrintable(error));
    QVERIFY(notes.isEmpty());
    QCOMPARE(window.document()->bodies().size(), std::size_t{1});
    const ObjectId body = window.document()->bodies().front().id;
    QCOMPARE(QString::fromStdString(window.document()->findBody(body)->name),
             QStringLiteral("Bracket"));
    QVERIFY(nearly(volumeOf(window, body), plateVolume(10.0)));
    QVERIFY(window.statusBar()->currentMessage().contains(
        QStringLiteral("Imported 1 body from bracket.step")));
    QCOMPARE(window.browser()->selectedBodies(), (std::vector<ObjectId>{body}));
    QVERIFY(action(window, "file.export")->isEnabled());
    QVERIFY(window.document()->canUndo());
    window.document()->undo();
    QVERIFY(window.document()->bodies().empty());
    window.document()->redo();
    QCOMPARE(window.document()->bodies().size(), std::size_t{1});

    // A file that is not a model says so, and changes nothing.
    const QString notes2 = m_directory.filePath(QStringLiteral("notes.step"));
    {
        QFile file(notes2);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("Not a model.\n");
    }
    QVERIFY(!window.importGeometry(notes2, &error));
    QVERIFY(error.contains(QStringLiteral("not a STEP file")));
    // The STEP reader logs what it could not parse; that is expected here, not a fault.
    const QStringList parsing = m_graphicsMessages.take();
    QVERIFY(parsing.isEmpty() || parsing.front().contains(QStringLiteral("StepFile")));
    QCOMPARE(window.document()->bodies().size(), std::size_t{1});

    // Offset Face, with the top face clicked in the 3D view, 5 up.
    window.viewport()->setStandardView(StandardView::Home);
    render(window);
    window.viewport()->clearSelection();
    QVERIFY(action(window, "solid.offsetFace")->isEnabled());
    QVERIFY(trigger(window, "solid.offsetFace"));
    FeaturePanel* offset = window.featurePanel();
    QVERIFY(offset != nullptr);
    QCOMPARE(offset->kind(), FeatureKind::OffsetFace);
    QCOMPARE(window.viewport()->selectionFilter(), lcad::viewport::SelectionFilter::Faces);
    QVERIFY(!okButton(offset)->isEnabled());
    click(window, at(window, 12.0, 32.0, 10.0));
    QCOMPARE(offset->feature().faces.size(), std::size_t{1});
    QCOMPARE(offset->feature().body, body);
    typeInto(offset, "distance", 5.0);
    QVERIFY2(offset->errorText().isEmpty(), qPrintable(offset->errorText()));
    render(window);
    keepFrame(window.grab().toImage(), "interchange-offset-preview");
    QTest::mouseClick(okButton(offset), Qt::LeftButton);
    QTRY_VERIFY(window.featurePanel() == nullptr);
    const ObjectId raised = window.document()->features().back().id;
    QCOMPARE(QString::fromStdString(window.document()->findFeature(raised)->name),
             QStringLiteral("Offset Face 1"));
    QVERIFY(nearly(volumeOf(window, body), plateVolume(15.0)));

    // Saved and opened again in a new window.
    const QString saved = m_directory.filePath(QStringLiteral("bracket.lcad"));
    QVERIFY2(window.saveTo(saved, &error), qPrintable(error));
    MainWindow reopened;
    QVERIFY2(reopened.loadFrom(saved, &error), qPrintable(error));
    QVERIFY(reopened.loadWarnings().isEmpty());
    QVERIFY(nearly(volumeOf(reopened, body), plateVolume(15.0)));

    // Exported to STEP from there, read back, and checked.
    const QString exported = m_directory.filePath(QStringLiteral("thicker.step"));
    ExportOptions options;
    options.format = ExportFormat::Step;
    QVERIFY2(reopened.exportGeometry(exported, options, &error), qPrintable(error));
    QVERIFY(reopened.statusBar()->currentMessage().contains(
        QStringLiteral("Exported 1 body to thicker.step")));
    const lcad::kernel::ImportResult back =
        lcad::kernel::importStep(std::filesystem::path(exported.toStdString()));
    QCOMPARE(back.bodies.size(), std::size_t{1});
    QCOMPARE(back.bodies[0].name, std::string("Bracket"));
    QVERIFY(lcad::kernel::checkGeometry(back.bodies[0].shape).empty());
    QVERIFY(nearly(lcad::kernel::volume(back.bodies[0].shape), plateVolume(15.0)));

    // And to STL, 3MF, and OBJ.
    for (const auto& [format, name] : {std::pair{ExportFormat::Stl, "thicker.stl"},
                                       std::pair{ExportFormat::ThreeMf, "thicker.3mf"},
                                       std::pair{ExportFormat::Obj, "thicker.obj"}}) {
        options.format = format;
        const QString path = m_directory.filePath(QString::fromLatin1(name));
        QVERIFY2(reopened.exportGeometry(path, options, &error), qPrintable(error));
        QVERIFY(QFileInfo(path).size() > 1000);
    }
    // Only what is selected, when that is asked for: nothing is.
    reopened.viewport()->clearSelection();
    options.selectedOnly = true;
    QVERIFY(!reopened.exportGeometry(m_directory.filePath(QStringLiteral("none.stl")), options,
                                     &error));
    QCOMPARE(error, QStringLiteral("There is no body to export."));
    QVERIFY(!QFileInfo::exists(m_directory.filePath(QStringLiteral("none.stl"))));
}

void InterchangeInterfaceTest::facesAreMovedAndDeleted() {
    MainWindow window;
    show(window);
    const ObjectId body = importPlate(window);
    QVERIFY(body.isValid());
    const int wall = faceWhere(window, body, isRound);
    QVERIFY(wall >= 0);

    // Move Face with the hole's wall selected first: it moves 10 along X.
    window.viewport()->setSelection({{body, SelectedItem::Kind::Face, wall}});
    QVERIFY(trigger(window, "solid.moveFace"));
    FeaturePanel* move = window.featurePanel();
    QVERIFY(move != nullptr);
    QCOMPARE(move->feature().faces.size(), std::size_t{1});
    // A round face is not moved square to itself: the X axis is the guess.
    QCOMPARE(move->feature().reference, std::string("X"));
    typeInto(move, "distance", 10.0);
    QVERIFY2(move->errorText().isEmpty(), qPrintable(move->errorText()));
    QTest::mouseClick(okButton(move), Qt::LeftButton);
    QTRY_VERIFY(window.featurePanel() == nullptr);
    const lcad::kernel::NamedShape moved = window.document()->namedBody(body);
    const int movedWall = faceWhere(window, body, isRound);
    const auto axis = lcad::kernel::axisOf(
        lcad::kernel::subShape(moved.shape, lcad::kernel::SubShapeKind::Face, movedWall));
    QVERIFY(axis.has_value());
    QVERIFY(nearly(axis->origin.x, 40.0));
    QVERIFY(nearly(volumeOf(window, body), plateVolume(10.0)));

    // Move Face on the top: the guess is square to it.
    const int top = faceWhere(window, body, [](const lcad::kernel::Shape& face) {
        const auto plane = lcad::kernel::planeOfFace(face);
        return plane.has_value() && plane->normal.z > 0.999;
    });
    window.viewport()->setSelection({{body, SelectedItem::Kind::Face, top}});
    QVERIFY(trigger(window, "solid.moveFace"));
    move = window.featurePanel();
    QCOMPARE(move->feature().reference, moved.faceNames[static_cast<std::size_t>(top)]);
    auto* direction = move->findChild<QComboBox*>(QStringLiteral("reference"));
    QCOMPARE(direction->currentText(), QStringLiteral("Square to the first face"));
    typeInto(move, "distance", 2.0);
    QTest::mouseClick(okButton(move), Qt::LeftButton);
    QTRY_VERIFY(window.featurePanel() == nullptr);
    QVERIFY(nearly(volumeOf(window, body), plateVolume(12.0)));

    // Delete Face on the hole: the plate is whole.
    window.viewport()->setSelection(
        {{body, SelectedItem::Kind::Face, faceWhere(window, body, isRound)}});
    QVERIFY(trigger(window, "solid.deleteFace"));
    FeaturePanel* remove = window.featurePanel();
    QVERIFY(remove != nullptr);
    QVERIFY(okButton(remove)->isEnabled());
    QTest::mouseClick(okButton(remove), Qt::LeftButton);
    QTRY_VERIFY(window.featurePanel() == nullptr);
    QVERIFY(nearly(volumeOf(window, body), 60.0 * 40.0 * 12.0));

    // Edited from the timeline: the move goes back to 5, and everything after follows.
    const ObjectId first = window.document()->features().front().id;
    QVERIFY(window.editFeature(first));
    QCOMPARE(window.featurePanel()->kind(), FeatureKind::MoveFace);
    QCOMPARE(window.featurePanel()->value(QStringLiteral("distance")), 10.0);
    QCOMPARE(window.featurePanel()->feature().reference, std::string("X"));
    typeInto(window.featurePanel(), "distance", 5.0);
    QTest::mouseClick(okButton(window.featurePanel()), Qt::LeftButton);
    QTRY_VERIFY(window.featurePanel() == nullptr);
    for (const auto& feature : window.document()->features()) {
        QVERIFY2(window.document()->featureError(feature.id).empty(),
                 window.document()->featureError(feature.id).c_str());
    }
    QVERIFY(nearly(volumeOf(window, body), 60.0 * 40.0 * 12.0));
    QCOMPARE(window.timeline()->describe(first).isEmpty(), false);
}

void InterchangeInterfaceTest::checkGeometryFindsProblemsAndRepairFixesThem() {
    MainWindow window;
    show(window);
    const ObjectId sound = importPlate(window);
    // The faces of a box, put in loose.
    const lcad::kernel::Shape box = lcad::kernel::makeBox(10.0, 20.0, 30.0);
    std::vector<lcad::kernel::Shape> faces;
    for (int i = 0; i < 6; ++i) {
        faces.push_back(lcad::kernel::subShape(box, lcad::kernel::SubShapeKind::Face, i));
    }
    const ObjectId loose = window.document()->addBody(
        "Loose", lcad::kernel::translated(lcad::kernel::makeCompound(faces), 100.0, 0.0, 0.0));
    window.viewport()->clearSelection();
    window.browser()->setSelection({}, {});

    // Check Geometry with nothing selected checks every body.
    QVERIFY(action(window, "inspect.checkGeometry")->isEnabled());
    QVERIFY(trigger(window, "inspect.checkGeometry"));
    GeometryCheckDialog* dialog = window.geometryCheckDialog();
    QVERIFY(dialog != nullptr);
    QVERIFY(dialog->isVisible());
    QTreeWidget* tree = dialog->tree();
    QCOMPARE(tree->topLevelItemCount(), 2);
    QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("Bracket: a sound solid."));
    QVERIFY(tree->topLevelItem(1)->text(0).startsWith(QStringLiteral("Loose: ")));
    QVERIFY(tree->topLevelItem(1)->childCount() >= 1);
    QVERIFY(
        window.statusBar()->currentMessage().contains(QStringLiteral("Problems found in 1 of 2")));
    keepFrame(dialog->grab().toImage(), "interchange-check");

    // Choosing the problem shows its edges in the 3D view.
    QTreeWidgetItem* problem = tree->topLevelItem(1)->child(0);
    QVERIFY(problem->text(0).contains(QStringLiteral("surfaces, not a solid")));
    tree->setCurrentItem(problem);
    const auto picked = window.viewport()->selection();
    QVERIFY(!picked.empty());
    QCOMPARE(picked.front().body, loose);

    // Repair Body starts a Repair of it; OK puts it in the timeline.
    auto* repair = dialog->findChild<QPushButton*>(QStringLiteral("repairBody"));
    QVERIFY(repair->isEnabled());
    QTest::mouseClick(repair, Qt::LeftButton);
    QTRY_VERIFY(window.featurePanel() != nullptr);
    QVERIFY(!dialog->isVisible());
    QCOMPARE(window.featurePanel()->kind(), FeatureKind::Repair);
    QCOMPARE(window.featurePanel()->feature().body, loose);
    QVERIFY2(window.featurePanel()->errorText().isEmpty(),
             qPrintable(window.featurePanel()->errorText()));
    QTest::mouseClick(okButton(window.featurePanel()), Qt::LeftButton);
    QTRY_VERIFY(window.featurePanel() == nullptr);
    QCOMPARE(lcad::kernel::countTopology(window.document()->findBody(loose)->shape).solids, 1);
    QVERIFY(nearly(volumeOf(window, loose), 6000.0));

    // Checked again: both are sound now.
    QVERIFY(trigger(window, "inspect.checkGeometry"));
    QCOMPARE(window.geometryCheckDialog()->tree()->topLevelItem(1)->text(0),
             QStringLiteral("Loose: a sound solid."));
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("no problems found")));
    Q_UNUSED(sound);
}

void InterchangeInterfaceTest::theExportDialogShowsOnlyWhatApplies() {
    ExportDialog dialog(3, 1);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto* format = dialog.findChild<QComboBox*>(QStringLiteral("exportFormat"));
    auto* bodies = dialog.findChild<QComboBox*>(QStringLiteral("exportBodies"));
    auto* resolution = dialog.findChild<QComboBox*>(QStringLiteral("meshResolution"));
    auto* deviation = dialog.findChild<QDoubleSpinBox*>(QStringLiteral("meshDeviation"));
    auto* angle = dialog.findChild<QDoubleSpinBox*>(QStringLiteral("meshAngle"));
    auto* text = dialog.findChild<QCheckBox*>(QStringLiteral("stlText"));
    auto* note = dialog.findChild<QLabel*>(QStringLiteral("exportNote"));
    QCOMPARE(bodies->count(), 2);
    QCOMPARE(bodies->itemText(0), QStringLiteral("The 3 bodies shown"));
    QCOMPARE(bodies->itemText(1), QStringLiteral("The body selected"));

    // STEP: no mesh settings.
    QCOMPARE(dialog.options().format, ExportFormat::Step);
    QVERIFY(!deviation->isVisible());
    QVERIFY(note->text().contains(QStringLiteral("exact solids")));

    // STL: the mesh settings, and text or binary.
    format->setCurrentIndex(format->findData(static_cast<int>(ExportFormat::Stl)));
    QVERIFY(deviation->isVisible());
    QVERIFY(text->isVisible());
    QVERIFY(note->text().contains(QStringLiteral("triangles")));
    QCOMPARE(resolution->currentText(), QStringLiteral("Medium"));
    resolution->setCurrentIndex(resolution->findText(QStringLiteral("Fine")));
    QMetaObject::invokeMethod(resolution, "activated", Q_ARG(int, resolution->currentIndex()));
    QCOMPARE(deviation->value(), 0.01);
    QCOMPARE(angle->value(), 5.0);
    // A value of its own makes it Custom.
    deviation->setValue(0.2);
    QCOMPARE(resolution->currentText(), QStringLiteral("Custom"));
    text->setChecked(true);
    bodies->setCurrentIndex(1);
    const ExportOptions chosen = dialog.options();
    QCOMPARE(chosen.format, ExportFormat::Stl);
    QVERIFY(chosen.selectedOnly);
    QCOMPARE(chosen.mesh.deviation, 0.2);
    QCOMPARE(chosen.stlEncoding, lcad::kernel::StlEncoding::Text);

    // 3MF: mesh settings, but STL's text choice is not for it.
    format->setCurrentIndex(format->findData(static_cast<int>(ExportFormat::ThreeMf)));
    QVERIFY(deviation->isVisible());
    QVERIFY(!text->isVisible());
    keepFrame(dialog.grab().toImage(), "interchange-export-dialog");

    // Options given back are shown again.
    ExportDialog again(1, 0);
    again.setOptions(chosen);
    QCOMPARE(again.options().mesh.deviation, 0.2);
    QCOMPARE(again.findChild<QComboBox*>(QStringLiteral("exportBodies"))->count(), 1);
    QCOMPARE(again.findChild<QComboBox*>(QStringLiteral("exportBodies"))->itemText(0),
             QStringLiteral("The body shown"));
    // With nothing selected there is nothing selected to export.
    QVERIFY(!again.options().selectedOnly);
}

QTEST_MAIN(InterchangeInterfaceTest)
#include "InterchangeInterfaceTest.moc"
