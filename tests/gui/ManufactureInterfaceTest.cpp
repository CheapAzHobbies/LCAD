// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/Commands.h>
#include <lcad/app/DrawingWorkspace.h>
#include <lcad/app/MachiningWorkspace.h>
#include <lcad/app/MainWindow.h>
#include <lcad/app/Theme.h>
#include <lcad/cam/Gcode.h>
#include <lcad/document/Document.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/Projection.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/kernel/Transform.h>
#include <lcad/viewport/ViewportWidget.h>

#include <FrameChecks.h>
#include <GraphicsMessages.h>
#include <GuiHelpers.h>

#include <QAction>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QListWidget>
#include <QSettings>
#include <QSlider>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>

#include <algorithm>
#include <cmath>

using lcad::app::MachiningWorkspace;
using lcad::app::MainWindow;
using lcad::app::Workspace;
using lcad::document::Document;
using lcad::document::ObjectId;
using lcad::document::OperationKind;
using lcad::document::OperationTarget;
using lcad::test::keepFrame;
using lcad::viewport::SelectedItem;

namespace {

// A plate of 80 by 60 by 12, a pocket of 40 by 30 and 6 deep in it, and a hole of 6 through it.
lcad::kernel::Shape bracket() {
    using namespace lcad::kernel;
    const NamedShape plate = nameFaces(makeBox(80.0, 60.0, 12.0), "plate");
    const NamedShape pocket =
        nameFaces(translated(makeBox(40.0, 30.0, 10.0), 10.0, 15.0, 6.0), "pocket");
    const NamedShape bore =
        nameFaces(translated(makeCylinder(3.0, 14.0), 65.0, 30.0, -1.0), "bore");
    return combine(combine(plate, pocket, Combination::Cut), bore, Combination::Cut).shape;
}

// The faces of a body by what they are.
struct Faces {
    int floor = -1;
    int bottom = -1;
    int hole = -1;
    std::string floorName;
    std::string bottomName;
    std::string holeName;
};

Faces facesOf(const Document& document, ObjectId body) {
    using namespace lcad::kernel;
    Faces faces;
    const NamedShape named = document.namedBody(body);
    for (int i = 0; i < subShapeCount(named.shape, SubShapeKind::Face); ++i) {
        const Shape face = subShape(named.shape, SubShapeKind::Face, i);
        const std::string& name = named.faceNames[static_cast<std::size_t>(i)];
        if (const auto plane = planeOfFace(face)) {
            if (plane->normal.z > 0.5 && std::abs(plane->origin.z - 6.0) < 1e-9) {
                faces.floor = i;
                faces.floorName = name;
            } else if (plane->normal.z < -0.5) {
                faces.bottom = i;
                faces.bottomName = name;
            }
        } else if (roundFaceAlongZ(face).has_value()) {
            faces.hole = i;
            faces.holeName = name;
        }
    }
    return faces;
}

} // namespace

// Manufacture as the window offers it, and the acceptance test of Phase 6: a dimensioned drawing
// from a model, machining operations on it, a simulation, and G-code through a checked post.
class ManufactureInterfaceTest : public QObject {
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

    void aModelBecomesADrawingAndGCode();
    void theWorkspaceHasItsOwnCommands();
    void operationsTakeWhatIsSelected();
    void gCodeIsRefusedWhenSomethingIsWrong();
    void rampsAndTabsAreOffered();
    void setupsAreSavedAndUndone();

private:
    static bool trigger(const MainWindow& window, const char* id) {
        return window.commands()->trigger(QString::fromLatin1(id));
    }
    static QAction* action(const MainWindow& window, const char* id) {
        return window.commands()->action(QString::fromLatin1(id));
    }
    static void show(MainWindow& window) {
        window.resize(1400, 900);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
    }
    // All four operations on the bracket.
    static void planBracket(MainWindow& window, ObjectId body) {
        const Faces faces = facesOf(*window.document(), body);
        MachiningWorkspace* machining = window.machining();
        machining->addOperation(OperationKind::Face, {});
        machining->addOperation(OperationKind::Pocket,
                                {OperationTarget{body, {}, faces.floorName}});
        machining->addOperation(OperationKind::Contour,
                                {OperationTarget{body, {}, faces.bottomName}});
        machining->addOperation(OperationKind::Drill, {OperationTarget{body, {}, faces.holeName}});
    }

    QTemporaryDir m_directory;
    lcad::test::GraphicsMessages m_graphicsMessages;
};

void ManufactureInterfaceTest::init() {
    QSettings().clear();
    lcad::app::Theme::instance().load();
    QVERIFY(m_directory.isValid());
}

void ManufactureInterfaceTest::cleanup() {
    const QStringList complaints = m_graphicsMessages.take();
    QVERIFY2(complaints.isEmpty(), qPrintable(complaints.join(QStringLiteral(" | "))));
}

void ManufactureInterfaceTest::aModelBecomesADrawingAndGCode() {
    MainWindow window;
    show(window);
    const ObjectId body = window.document()->addBody("Bracket", bracket());

    // A drawing with a dimension on it.
    window.setWorkspace(Workspace::Drawing);
    QTRY_VERIFY(!window.drawings()->view()->sheet().views.empty());
    window.drawings()->view()->fit();
    const auto& boxes = window.drawings()->view()->sheet().views;
    const auto top = std::find_if(boxes.begin(), boxes.end(), [&](const auto& box) {
        return window.drawings()->drawing()->views[1].id == box.view;
    });
    QVERIFY(top != boxes.end());
    QVERIFY(trigger(window, "sketch.dimension"));
    QTest::mouseClick(window.drawings()->view(), Qt::LeftButton, Qt::NoModifier,
                      window.drawings()
                          ->view()
                          ->toWidget({(top->left + top->right) / 2.0, top->bottom})
                          .toPoint());
    QCOMPARE(window.drawings()->view()->sheet().dimensions.size(), std::size_t{1});
    QVERIFY(qAbs(window.drawings()->view()->sheet().dimensions[0].value - 80.0) < 1e-6);
    QString error;
    const QString pdf = m_directory.filePath(QStringLiteral("bracket.pdf"));
    QVERIFY2(window.drawings()->exportTo(pdf, &error), qPrintable(error));

    // Operations on the model, in a setup that came with a first look.
    window.setWorkspace(Workspace::Manufacture);
    MachiningWorkspace* machining = window.machining();
    QVERIFY(machining->setup() != nullptr);
    QCOMPARE(window.document()->tools().size(), std::size_t{4});
    QCOMPARE(window.inspectorPanel(), machining);
    planBracket(window, body);
    QCOMPARE(machining->setup()->operations.size(), std::size_t{4});
    QVERIFY2(!machining->program().hasErrors(), qPrintable(machining->problemsText()));
    QCOMPARE(machining->program().paths.size(), std::size_t{4});
    // After the panels have laid themselves out.
    QTest::qWait(100);
    keepFrame(window.grab().toImage(), "manufacture-toolpaths");

    // A simulation, whose stock takes the bodies' place.
    QVERIFY(trigger(window, "manufacture.simulate"));
    QVERIFY(machining->isSimulating());
    QVERIFY2(!machining->report().hasErrors(), qPrintable(machining->problemsText()));
    QVERIFY(machining->report().cuttingSeconds > 10.0);
    QVERIFY(machining->report().removedVolume > 1000.0);
    QVERIFY(machining->summaryText().contains(QStringLiteral("cm³")));
    QTest::qWait(100);
    keepFrame(window.grab().toImage(), "manufacture-simulated");

    // Back to any move along the way, and forward again.
    QSlider* slider = machining->moveSlider();
    QVERIFY(slider->isEnabled());
    slider->setValue(slider->maximum() / 3);
    keepFrame(window.grab().toImage(), "manufacture-midway");
    slider->setValue(slider->maximum());

    // G-code, through its post, that a reader of the dialect agrees with.
    const QString path = m_directory.filePath(QStringLiteral("bracket.nc"));
    QVERIFY2(machining->exportTo(path, &error), qPrintable(error));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const std::string text = file.readAll().toStdString();
    const auto wrong =
        lcad::cam::verifyGcode(text, machining->program(), lcad::document::PostDialect::Grbl);
    QVERIFY(wrong.empty());
    QVERIFY(text.find("M3") != std::string::npos);
    QVERIFY(text.find("M6") == std::string::npos);
    // Kept with the frames of a run, for a person to read.
    const QString artifacts = qEnvironmentVariable("LCAD_TEST_ARTIFACTS");
    if (!artifacts.isEmpty()) {
        QFile::remove(artifacts + QStringLiteral("/bracket.nc"));
        QFile::copy(path, artifacts + QStringLiteral("/bracket.nc"));
    }

    // And another dialect at the turn of a box.
    QComboBox* dialect = machining->dialectBox();
    dialect->setCurrentIndex(1);
    emit dialect->activated(1);
    QCOMPARE(machining->setup()->machine.dialect, lcad::document::PostDialect::LinuxCnc);
    const auto cnc = machining->gcode(&error);
    QVERIFY2(cnc.has_value(), qPrintable(error));
    QVERIFY(cnc->find("T1 M6") != std::string::npos);
}

void ManufactureInterfaceTest::theWorkspaceHasItsOwnCommands() {
    MainWindow window;
    show(window);
    window.document()->addBody("Bracket", bracket());
    auto* tools = window.findChild<QToolBar*>(QStringLiteral("machiningToolbar"));
    auto* modeling = window.findChild<QToolBar*>(QStringLiteral("modelingToolbar"));
    QVERIFY(!tools->isVisible());
    QVERIFY(!action(window, "manufacture.pocket")->isEnabled());

    QVERIFY(trigger(window, "workspace.manufacture"));
    QTRY_VERIFY(tools->isVisible());
    QVERIFY(!modeling->isVisible());
    // The 3D view stays, as it is.
    QVERIFY(window.viewport()->isVisible());
    QVERIFY(action(window, "view.home")->isEnabled());
    QVERIFY(!action(window, "solid.revolve")->isEnabled());
    QVERIFY(action(window, "manufacture.pocket")->isEnabled());
    QVERIFY(action(window, "manufacture.simulate")->isEnabled());

    // Leaving takes the toolpaths and the simulation away, and leaves the setup.
    window.machining()->addOperation(OperationKind::Face, {});
    window.machining()->simulate();
    QVERIFY(window.machining()->isSimulating());
    QVERIFY(trigger(window, "workspace.design"));
    QVERIFY(!window.machining()->isSimulating());
    QCOMPARE(window.document()->setups().size(), std::size_t{1});
    QTRY_VERIFY(modeling->isVisible());
}

void ManufactureInterfaceTest::operationsTakeWhatIsSelected() {
    MainWindow window;
    show(window);
    const ObjectId body = window.document()->addBody("Bracket", bracket());
    const Faces faces = facesOf(*window.document(), body);
    window.setWorkspace(Workspace::Manufacture);

    // Click the pocket's floor in the 3D view, as a user would, through the selection.
    window.viewport()->setSelection(
        {SelectedItem{body, SelectedItem::Kind::Face, faces.floor, {}}});
    QVERIFY(trigger(window, "manufacture.pocket"));
    MachiningWorkspace* machining = window.machining();
    QVERIFY(machining->operation() != nullptr);
    QCOMPARE(machining->operation()->kind, OperationKind::Pocket);
    QCOMPARE(machining->operation()->targets.size(), std::size_t{1});
    QCOMPARE(machining->operation()->targets[0].face, faces.floorName);
    QCOMPARE(QString::fromStdString(machining->operation()->name), QStringLiteral("Pocket 1"));
    QCOMPARE(machining->operationList()->count(), 1);

    // Its fields change it, as one undo step each.
    const QDoubleSpinBox* field = machining->feedField();
    QCOMPARE(field->value(), 600.0);
    machining->feedField()->setValue(450.0);
    QCOMPARE(machining->operation()->feed, 450.0);
    QCOMPARE(machining->program().paths[0].moves.back().kind, lcad::cam::MoveKind::Rapid);
    QVERIFY(action(window, "edit.undo")->text().contains(QStringLiteral("Change Operation")));
    QVERIFY(trigger(window, "edit.undo"));
    QCOMPARE(machining->operation()->feed, 600.0);

    // The stock's margin moves zero with it.
    const double before = machining->program().work.stock.min.x;
    machining->stockSideField()->setValue(8.0);
    QVERIFY(qAbs(machining->program().work.stock.min.x - (before - 5.0)) < 1e-9);

    // Delete takes the operation selected.
    QVERIFY(trigger(window, "edit.delete"));
    QVERIFY(machining->setup()->operations.empty());
    QVERIFY(machining->operation() == nullptr);
    QVERIFY(machining->program().paths.empty());
}

void ManufactureInterfaceTest::gCodeIsRefusedWhenSomethingIsWrong() {
    MainWindow window;
    show(window);
    const ObjectId body = window.document()->addBody("Bracket", bracket());
    window.setWorkspace(Workspace::Manufacture);
    MachiningWorkspace* machining = window.machining();
    const QString path = m_directory.filePath(QStringLiteral("refused.nc"));
    QString error;

    // Nothing to write.
    QVERIFY(!machining->exportTo(path, &error));
    QVERIFY2(error.contains(QStringLiteral("no toolpaths")), qPrintable(error));

    planBracket(window, body);
    QVERIFY(machining->exportTo(path, &error));

    // A feed the machine does not have: the panel says so, and nothing is written.
    machining->selectOperation(machining->setup()->operations[1].id);
    machining->feedField()->setValue(5000.0);
    QVERIFY(machining->program().hasErrors());
    QVERIFY(machining->problemsText().contains(QStringLiteral("faster")));
    QFile::remove(path);
    QVERIFY(!machining->exportTo(path, &error));
    QVERIFY2(error.contains(QStringLiteral("faster")), qPrintable(error));
    QVERIFY(!QFile::exists(path));
    machining->feedField()->setValue(600.0);
    QVERIFY(machining->exportTo(path, &error));

    // A body put under where the tool goes: the plan knows nothing of it, the simulation does.
    window.document()->addBody(
        "Clamp", lcad::kernel::translated(lcad::kernel::makeBox(10.0, 10.0, 8.0), 35.0, 25.0, 0.0));
    QFile::remove(path);
    QVERIFY(!machining->exportTo(path, &error));
    QVERIFY2(error.contains(QStringLiteral("cuts into the part")), qPrintable(error));
    QVERIFY(!QFile::exists(path));
}

void ManufactureInterfaceTest::rampsAndTabsAreOffered() {
    MainWindow window;
    show(window);
    const ObjectId body = window.document()->addBody("Bracket", bracket());
    window.setWorkspace(Workspace::Manufacture);
    planBracket(window, body);
    MachiningWorkspace* machining = window.machining();
    auto* ramp = machining->findChild<QDoubleSpinBox*>(QStringLiteral("operationRamp"));
    auto* tabs = machining->findChild<QDoubleSpinBox*>(QStringLiteral("operationTabs"));
    QVERIFY(ramp != nullptr && tabs != nullptr);
    const auto choose = [machining](OperationKind kind) {
        const auto& operations = machining->setup()->operations;
        for (std::size_t i = 0; i < operations.size(); ++i) {
            if (operations[i].kind == kind) {
                machining->operationList()->setCurrentRow(static_cast<int>(i));
                return &operations[i];
            }
        }
        return static_cast<const lcad::document::MachiningOperation*>(nullptr);
    };

    // A contour has both, and starts out ramping, and warns of what comes loose until it has tabs.
    QVERIFY(choose(OperationKind::Contour) != nullptr);
    QVERIFY(ramp->isVisible());
    QVERIFY(tabs->isVisible());
    QCOMPARE(ramp->value(), 3.0);
    QVERIFY(machining->problemsText().contains(QStringLiteral("comes loose")));
    tabs->setValue(4.0);
    QCOMPARE(choose(OperationKind::Contour)->tabs, 4);
    QTRY_VERIFY(!machining->problemsText().contains(QStringLiteral("comes loose")));
    QVERIFY2(!machining->program().hasErrors(), qPrintable(machining->problemsText()));
    ramp->setValue(10.0);
    QCOMPARE(choose(OperationKind::Contour)->rampAngle, 10.0);
    window.document()->undo();
    QCOMPARE(choose(OperationKind::Contour)->rampAngle, 3.0);

    // A hole has neither, and a pocket has only the ramp.
    QVERIFY(choose(OperationKind::Drill) != nullptr);
    QVERIFY(!ramp->isVisible());
    QVERIFY(!tabs->isVisible());
    QVERIFY(choose(OperationKind::Pocket) != nullptr);
    QVERIFY(ramp->isVisible());
    QVERIFY(!tabs->isVisible());
}

void ManufactureInterfaceTest::setupsAreSavedAndUndone() {
    const QString file = m_directory.filePath(QStringLiteral("setup.lcad"));
    QString error;
    {
        MainWindow window;
        show(window);
        const ObjectId body = window.document()->addBody("Bracket", bracket());
        window.setWorkspace(Workspace::Manufacture);
        planBracket(window, body);
        QVERIFY(window.saveTo(file, &error));
    }
    MainWindow window;
    show(window);
    QVERIFY(window.loadFrom(file, &error));
    window.setWorkspace(Workspace::Manufacture);
    MachiningWorkspace* machining = window.machining();
    QVERIFY(machining->setup() != nullptr);
    QCOMPARE(machining->setup()->operations.size(), std::size_t{4});
    QCOMPARE(machining->program().paths.size(), std::size_t{4});
    QCOMPARE(machining->operationList()->count(), 4);

    // Each document has its own.
    window.newDocument();
    QVERIFY(machining->setup() == nullptr);
    QVERIFY(machining->program().paths.empty());

    // A document that goes leaves nothing holding on to it: the sanitizers would see that here.
    window.closeDocument(window.documentCount() - 1);
    QVERIFY(machining->setup() != nullptr);
    window.closeDocument(0);
    QVERIFY(machining->setup() == nullptr);
    QVERIFY(window.loadFrom(file, &error));
    QCOMPARE(machining->program().paths.size(), std::size_t{4});
}

QTEST_MAIN(ManufactureInterfaceTest)
#include "ManufactureInterfaceTest.moc"
