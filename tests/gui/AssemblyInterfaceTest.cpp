// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/AssemblyDialogs.h>
#include <lcad/app/BrowserPanel.h>
#include <lcad/app/Commands.h>
#include <lcad/app/FeaturePanel.h>
#include <lcad/app/JointPanel.h>
#include <lcad/app/MainWindow.h>
#include <lcad/app/MeasurePanel.h>
#include <lcad/app/MovePanel.h>
#include <lcad/app/Paths.h>
#include <lcad/app/PropertiesPanel.h>
#include <lcad/app/Recovery.h>
#include <lcad/app/Theme.h>
#include <lcad/document/File.h>
#include <lcad/document/Links.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/Plane.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/kernel/Transform.h>
#include <lcad/sketch/Edit.h>
#include <lcad/viewport/ViewportWidget.h>

#include <FrameChecks.h>
#include <GraphicsMessages.h>
#include <GuiHelpers.h>

#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

#include <cmath>
#if defined(Q_OS_WIN)
#include <windows.h>
#endif

#include <csignal>
#include <filesystem>
#include <functional>
#include <string>

using lcad::app::BrowserPanel;
using lcad::app::FeaturePanel;
using lcad::app::JointPanel;
using lcad::app::MainWindow;
using lcad::app::toPath;
using lcad::document::FeatureKind;
using lcad::document::ObjectId;
using lcad::viewport::SelectedItem;

namespace {

bool nearly(double a, double b, double tolerance = 1e-6) {
    return std::abs(a - b) <= tolerance * std::max(1.0, std::abs(b));
}

// A part made in a component of its own: a box, placed somewhere.
struct Part {
    ObjectId component;
    ObjectId occurrence;
    ObjectId body;
};

Part makePart(lcad::document::Document& document, const std::string& name, double x, double y,
              double z, const lcad::kernel::Placement& placement = {}) {
    Part part;
    part.component = document.addComponent(name, {}, placement);
    part.occurrence = document.occurrencesOf(part.component).front();
    part.body = document.addBody(name, lcad::kernel::makeBox(x, y, z), part.component);
    return part;
}

// The face of a body whose outward normal points along `z`.
int faceFacing(const lcad::document::Document& document, ObjectId body, double z) {
    const lcad::document::Body* found = document.findBody(body);
    for (int i = 0; i < lcad::kernel::subShapeCount(found->shape, lcad::kernel::SubShapeKind::Face);
         ++i) {
        const auto frame = lcad::kernel::frameOn(
            lcad::kernel::subShape(found->shape, lcad::kernel::SubShapeKind::Face, i));
        if (frame.has_value() && std::abs(frame->zAxis.z - z) < 1e-9) {
            return i;
        }
    }
    return -1;
}

// Where a part's body is in the root's coordinates.
lcad::kernel::BoundingBox boundsOf(const lcad::document::Document& document, const Part& part) {
    const auto bounds = lcad::kernel::boundingBox(lcad::kernel::placed(
        document.findBody(part.body)->shape, document.placementOf({part.occurrence})));
    return bounds.value_or(lcad::kernel::BoundingBox{});
}

} // namespace

// Assemblies as the window offers them: components and working on one, joints set up in their
// panel and shown moving, components moved by hand, overlaps found, mass weighed, linked parts
// inserted, listed, and packaged, and unsaved work restored after a crash. Also the arrow a
// pull's distance is dragged by.
class AssemblyInterfaceTest : public QObject {
    Q_OBJECT

public:
    static void initMain() {
        // Run again as a child that crashes on purpose: see unsavedWorkSurvivesACrash().
        const QString crashIn = qEnvironmentVariable("LCAD_TEST_CRASH_IN");
        if (!crashIn.isEmpty()) {
            crashAfterKeepingWork(crashIn);
        }
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

    void aNewComponentIsWorkedOn();
    void bodiesBecomeAComponent();
    void twoPartsAreJoinedInThePanelAndAnimated();
    void aComponentIsMovedByHand();
    void componentsAreMeasuredWhereTheyAre();
    void overlapsAreFoundAndMassIsWeighed();
    void aLinkedPartIsInsertedListedAndPackaged();
    void unsavedWorkSurvivesACrash();
    void thePullArrowIsDraggedAndTyped();

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
    // A picture of the window once its layout has caught up with what changed.
    static void keep(MainWindow& window, const char* name) {
        QTest::qWait(50);
        render(window);
        lcad::test::keepFrame(window.grab().toImage(), name);
    }
    template <typename Panel>
    static QPushButton* okButton(Panel* panel) {
        for (QPushButton* button : panel->template findChildren<QPushButton*>()) {
            if (button->text().remove(QLatin1Char('&')) == QStringLiteral("OK")) {
                return button;
            }
        }
        return nullptr;
    }
    // Writes a document with unsaved work into a recovery folder, and dies as a crash would:
    // no cleaning up, the lock left behind.
    [[noreturn]] static void crashAfterKeepingWork(const QString& folder) {
        lcad::app::RecoveryStore store(folder);
        if (store.open()) {
            lcad::document::Document document;
            document.addBody("Unsaved", lcad::kernel::makeBox(7.0, 8.0, 9.0));
            store.save(QStringLiteral("1"), document, QStringLiteral("/somewhere/part.lcad"),
                       QStringLiteral("part"));
        }
#if defined(Q_OS_WIN)
        TerminateProcess(GetCurrentProcess(), 9);
#else
        std::raise(SIGKILL);
#endif
        std::abort();
    }

    QTemporaryDir m_directory;
    lcad::test::GraphicsMessages m_graphicsMessages;
};

void AssemblyInterfaceTest::init() {
    QTest::failOnWarning(QRegularExpression(QStringLiteral("OpenGL|QRhi|context|surface"),
                                            QRegularExpression::CaseInsensitiveOption));
    QSettings().clear();
    lcad::app::Theme::instance().load();
    QVERIFY(m_directory.isValid());
}

void AssemblyInterfaceTest::cleanup() {
    const QStringList complaints = m_graphicsMessages.take();
    QVERIFY2(complaints.isEmpty(), qPrintable(complaints.join(QStringLiteral(" | "))));
}

void AssemblyInterfaceTest::aNewComponentIsWorkedOn() {
    MainWindow window;
    show(window);
    QVERIFY(trigger(window, "assembly.newComponent"));

    lcad::document::Document* document = window.document();
    QCOMPARE(document->components().size(), std::size_t{1});
    const ObjectId component = document->components().front().id;
    QCOMPARE(window.activeComponent(), component);
    QCOMPARE(window.activePath().size(), std::size_t{1});
    // It is listed, in bold, under the document.
    QTreeWidgetItem* entry = window.browser()->occurrenceItem(window.activePath());
    QVERIFY(entry != nullptr);
    QCOMPARE(entry->text(0), QStringLiteral("Component 1:1"));
    QVERIFY(entry->font(0).bold());

    // A box made now goes into it.
    window.startPrimitive(lcad::app::PrimitiveKind::Box);
    window.acceptCommand();
    QCOMPARE(document->bodies().size(), std::size_t{1});
    QCOMPARE(document->bodies().front().component, component);
    QVERIFY(window.browser()->bodyItem(document->bodies().front().id, window.activePath()) !=
            nullptr);

    // Double-clicking the document's entry works on the whole again. (The entry is looked up
    // again: the browser lists the components afresh when the document changes.)
    const std::vector<ObjectId> path = window.activePath();
    emit window.browser()->occurrenceActivated({});
    QVERIFY(window.activePath().empty());
    entry = window.browser()->occurrenceItem(path);
    QVERIFY(entry != nullptr);
    QVERIFY(!entry->font(0).bold());

    // Undo takes the box, and then the component; working on it ends with it.
    window.activateOccurrence({document->occurrences().front().id});
    document->undo();
    document->undo();
    QVERIFY(document->components().empty());
    QVERIFY(window.activePath().empty());
}

void AssemblyInterfaceTest::bodiesBecomeAComponent() {
    MainWindow window;
    show(window);
    const ObjectId box = window.document()->addBody("Box", lcad::kernel::makeBox(10.0, 10.0, 10.0));
    window.browser()->setSelection({box}, {});
    emit window.browser()->bodySelectionChanged();
    QVERIFY(action(window, "assembly.makeComponent")->isEnabled());

    QVERIFY(trigger(window, "assembly.makeComponent"));

    const lcad::document::Body* body = window.document()->findBody(box);
    QVERIFY(body->component.isValid());
    QCOMPARE(window.document()->findComponent(body->component)->name, std::string("Box"));
    // Listed under its occurrence now, not among the document's own bodies.
    QTreeWidgetItem* listed = window.browser()->bodyItem(box);
    QVERIFY(listed != nullptr);
    QCOMPARE(BrowserPanel::pathOf(listed).size(), std::size_t{1});
    QCOMPARE(window.browser()->tree()->topLevelItem(0)->child(1)->childCount(), 0);
}

void AssemblyInterfaceTest::twoPartsAreJoinedInThePanelAndAnimated() {
    MainWindow window;
    show(window);
    lcad::document::Document* document = window.document();
    const Part base = makePart(*document, "Base", 40.0, 40.0, 10.0);
    document->setOccurrenceGrounded(base.occurrence, true);
    const Part arm =
        makePart(*document, "Arm", 30.0, 6.0, 4.0, lcad::kernel::translation({80.0, 20.0, 15.0}));
    render(window);

    QVERIFY(trigger(window, "assembly.joint"));
    JointPanel* panel = window.jointPanel();
    QVERIFY(panel != nullptr);
    QCOMPARE(panel->pickingSide(), 0);
    QVERIFY(!okButton(panel)->isEnabled());

    // The arm's underside, then the base's top, clicked as the 3D view would pick them.
    window.viewport()->setSelection({{arm.body,
                                      SelectedItem::Kind::Face,
                                      faceFacing(*document, arm.body, -1.0),
                                      {arm.occurrence}}});
    QCOMPARE(panel->pickingSide(), 1);
    QVERIFY(panel->findChild<QLabel*>(QStringLiteral("jointMoving"))
                ->text()
                .contains(QStringLiteral("Arm:1")));
    window.viewport()->setSelection({{base.body,
                                      SelectedItem::Kind::Face,
                                      faceFacing(*document, base.body, 1.0),
                                      {base.occurrence}}});
    QCOMPARE(panel->pickingSide(), -1);
    QVERIFY2(panel->errorText().isEmpty(), qPrintable(panel->errorText()));
    QVERIFY(okButton(panel)->isEnabled());
    // Two flat faces suggest a rigid joint; a revolute one is chosen instead.
    QCOMPARE(panel->joint().kind, lcad::document::JointKind::Rigid);
    panel->setKind(lcad::document::JointKind::Revolute);
    auto* rotation = panel->findChild<QDoubleSpinBox*>(QStringLiteral("jointValue0"));
    QVERIFY(rotation != nullptr && rotation->isVisible());
    rotation->setValue(90.0);
    render(window);
    keep(window, "joint-preview");
    // The preview has the document as it was.
    QVERIFY(document->joints().empty());

    QTest::mouseClick(okButton(panel), Qt::LeftButton);
    QVERIFY(window.jointPanel() == nullptr);
    QCOMPARE(document->joints().size(), std::size_t{1});
    const lcad::document::Joint joint = document->joints().front();
    QVERIFY(document->jointError(joint.id).empty());
    const lcad::kernel::BoundingBox placed = boundsOf(*document, arm);
    QVERIFY(nearly(placed.min.z, 10.0));
    // Turned a quarter: it lies along Y now, over the middle of the base.
    QVERIFY(nearly(placed.max.y - placed.min.y, 30.0));
    QVERIFY(nearly((placed.min.x + placed.max.x) / 2.0, 20.0));
    // Listed under Joints, with the document's own joints.
    QVERIFY(window.browser()->jointItem(joint.id) != nullptr);

    // Shown in motion for a while, and back where it was; the document is not changed.
    window.browser()->setAssemblySelection({}, {joint.id});
    emit window.browser()->bodySelectionChanged();
    QVERIFY(action(window, "assembly.animate")->isEnabled());
    QVERIFY(trigger(window, "assembly.animate"));
    QVERIFY(window.isAnimating());
    QTest::qWait(400);
    render(window);
    QTRY_VERIFY_WITH_TIMEOUT(!window.isAnimating(), 6000);
    QCOMPARE(document->joints().front().values[0], 90.0);
    QVERIFY(document->canUndo());

    // Edited from the browser: the panel opens with its values.
    emit window.browser()->jointActivated(joint.id);
    QVERIFY(window.jointPanel() != nullptr);
    QCOMPARE(window.jointPanel()->joint().kind, lcad::document::JointKind::Revolute);
    QCOMPARE(window.jointPanel()->joint().values[0], 90.0);
    window.cancelCommand();
    QVERIFY(window.jointPanel() == nullptr);

    // The properties say what it is.
    emit window.browser()->bodySelectionChanged();
    QCOMPARE(window.properties()->heading(), QString::fromStdString(joint.name));
    QVERIFY(window.properties()->rows().contains(QStringLiteral("Holds: Yes")));
}

void AssemblyInterfaceTest::aComponentIsMovedByHand() {
    MainWindow window;
    show(window);
    lcad::document::Document* document = window.document();
    const Part part = makePart(*document, "Block", 10.0, 10.0, 10.0);
    window.browser()->setAssemblySelection({{part.occurrence}}, {});
    emit window.browser()->bodySelectionChanged();
    QVERIFY(action(window, "assembly.move")->isEnabled());
    QVERIFY(trigger(window, "assembly.move"));
    lcad::app::MovePanel* panel = window.movePanel();
    QVERIFY(panel != nullptr);
    panel->setValue(0, 25.0);
    panel->setValue(5, 90.0);
    render(window);
    // A preview only, so far.
    QVERIFY(nearly(boundsOf(*document, part).min.x, 0.0));
    QTest::mouseClick(okButton(panel), Qt::LeftButton);
    QVERIFY(window.movePanel() == nullptr);
    const lcad::kernel::BoundingBox moved = boundsOf(*document, part);
    // Turned a quarter about Z at its origin, then moved 25 along X: from 15 to 25.
    QVERIFY(nearly(moved.min.x, 15.0));
    QVERIFY(nearly(moved.max.x, 25.0));
    QCOMPARE(QString::fromStdString(document->undoLabel()), QStringLiteral("Move Block:1"));
}

void AssemblyInterfaceTest::componentsAreMeasuredWhereTheyAre() {
    MainWindow window;
    show(window);
    lcad::document::Document* document = window.document();
    // Two cubes, each at its component's origin, placed 30 mm apart.
    const Part left = makePart(*document, "Left", 10.0, 10.0, 10.0);
    const Part right =
        makePart(*document, "Right", 10.0, 10.0, 10.0, lcad::kernel::translation({30.0, 0.0, 0.0}));
    render(window);
    QVERIFY(trigger(window, "inspect.measure"));
    window.viewport()->setSelection({{left.body, SelectedItem::Kind::Body, -1, {left.occurrence}}});
    window.viewport()->setSelection(
        {{right.body, SelectedItem::Kind::Body, -1, {right.occurrence}}});
    QVERIFY(window.measurePanel() != nullptr);
    // 20 mm between them, as placed: not 0, as their bodies are in their own coordinates.
    QVERIFY2(window.measurePanel()->rows().contains(QStringLiteral("Distance: 20 mm")),
             qPrintable(window.measurePanel()->rows().join(QLatin1Char('|'))));
    QVERIFY(window.measurePanel()->rows().contains(QStringLiteral("Along X: 20 mm")));
}

void AssemblyInterfaceTest::overlapsAreFoundAndMassIsWeighed() {
    MainWindow window;
    show(window);
    lcad::document::Document* document = window.document();
    const Part first = makePart(*document, "First", 10.0, 10.0, 10.0);
    const Part second =
        makePart(*document, "Second", 10.0, 10.0, 10.0, lcad::kernel::translation({8.0, 0.0, 0.0}));
    makePart(*document, "Apart", 10.0, 10.0, 10.0, lcad::kernel::translation({50.0, 0.0, 0.0}));
    render(window);

    QVERIFY(trigger(window, "inspect.interference"));
    lcad::app::InterferenceDialog* dialog = window.interferenceDialog();
    QVERIFY(dialog != nullptr && dialog->isVisible());
    QCOMPARE(dialog->tree()->topLevelItemCount(), 1);
    QVERIFY(dialog->tree()->topLevelItem(0)->text(0).contains(QStringLiteral("First:1")));
    QVERIFY(dialog->tree()->topLevelItem(0)->text(1).startsWith(QStringLiteral("200")));
    dialog->tree()->setCurrentItem(dialog->tree()->topLevelItem(0));
    render(window);
    keep(window, "interference");
    dialog->close();

    // Steel, 7.85 g/cm3: a 10 mm cube is 7.85 g.
    document->setBodyMaterial(first.body, "Steel");
    document->setBodyMaterial(second.body, "Steel");
    window.viewport()->setSelection(
        {{first.body, SelectedItem::Kind::Body, -1, {first.occurrence}},
         {second.body, SelectedItem::Kind::Body, -1, {second.occurrence}}});
    QVERIFY(trigger(window, "inspect.physical"));
    lcad::app::MassDialog* mass = window.massDialog();
    QVERIFY(mass != nullptr && mass->isVisible());
    QCOMPARE(mass->table()->rowCount(), 2);
    QVERIFY2(mass->totals().contains(QStringLiteral("Mass: 15.7 g")),
             qPrintable(mass->totals().join(QLatin1Char('|'))));
    QVERIFY(mass->totals().contains(QStringLiteral("Center of mass: 9, 5, 5 mm")));
    mass->close();

    // The material is chosen in the properties of one body.
    window.viewport()->setSelection(
        {{first.body, SelectedItem::Kind::Body, -1, {first.occurrence}}});
    auto* material = window.properties()->findChild<QComboBox*>(QStringLiteral("bodyMaterial"));
    QVERIFY(material != nullptr);
    QCOMPARE(material->currentText(), QStringLiteral("Steel"));
    QVERIFY(window.properties()->rows().contains(QStringLiteral("Mass: 7.85 g")));
    material->setCurrentIndex(material->findData(QStringLiteral("Aluminum")));
    emit material->activated(material->currentIndex());
    QCOMPARE(document->findBody(first.body)->material, std::string("Aluminum"));
}

void AssemblyInterfaceTest::aLinkedPartIsInsertedListedAndPackaged() {
    const QString partFile = m_directory.filePath(QStringLiteral("parts/wheel.lcad"));
    QVERIFY(QDir().mkpath(m_directory.filePath(QStringLiteral("parts"))));
    {
        lcad::document::Document part;
        part.addBody("Tire", lcad::kernel::makeCylinder(20.0, 8.0));
        lcad::document::saveDocument(part, toPath(partFile));
    }
    MainWindow window;
    show(window);
    QString error;
    QVERIFY(window.saveTo(m_directory.filePath(QStringLiteral("car.lcad")), &error));
    QVERIFY2(window.insertLinkedPart(partFile, &error), qPrintable(error));
    lcad::document::Document* document = window.document();
    QCOMPARE(document->components().size(), std::size_t{1});
    QCOMPARE(document->components().front().link, std::string("parts/wheel.lcad"));
    // Inserted again, it is the same part appearing twice.
    QVERIFY(window.insertLinkedPart(partFile, &error));
    QCOMPARE(document->components().size(), std::size_t{1});
    QCOMPARE(document->occurrences().size(), std::size_t{2});
    render(window);
    // A linked part is changed in its own file, not worked on here.
    window.activateOccurrence({document->occurrences().front().id});
    QVERIFY(window.activePath().empty());

    QVERIFY(trigger(window, "file.links"));
    lcad::app::LinksDialog* links = window.linksDialog();
    QVERIFY(links != nullptr && links->isVisible());
    QCOMPARE(links->tree()->topLevelItemCount(), 1);
    QCOMPARE(links->tree()->topLevelItem(0)->text(1), QStringLiteral("Up to date"));

    // The part changes in its own file: the list says so, and Update takes it.
    {
        lcad::document::Document part;
        part.addBody("Tire", lcad::kernel::makeCylinder(25.0, 8.0));
        lcad::document::saveDocument(part, toPath(partFile));
    }
    window.showLinks();
    QVERIFY(links->tree()->topLevelItem(0)->text(1).startsWith(QStringLiteral("Changed")));
    QVERIFY(window.updateLinks().isEmpty());
    QCOMPARE(links->tree()->topLevelItem(0)->text(1), QStringLiteral("Up to date"));
    links->close();

    // Packaged, the project opens elsewhere with its part.
    const QString folder = m_directory.filePath(QStringLiteral("package"));
    QStringList problems;
    QVERIFY(window.packageTo(folder, &error, &problems));
    QVERIFY(problems.isEmpty());
    QVERIFY(QFileInfo::exists(folder + QStringLiteral("/car.lcad")));
    QVERIFY(QFileInfo::exists(folder + QStringLiteral("/wheel.lcad")));

    // Opened without its part, the package says so, and shows the copy it kept.
    QVERIFY(QFile::remove(folder + QStringLiteral("/wheel.lcad")));
    QVERIFY(window.loadFrom(folder + QStringLiteral("/car.lcad"), &error));
    QCOMPARE(window.loadWarnings().size(), 1);
    QVERIFY2(window.loadWarnings().front().contains(QStringLiteral("cannot be found")),
             qPrintable(window.loadWarnings().front()));
    QCOMPARE(window.document()->bodies().size(), std::size_t{1});
    QVERIFY(!window.document()->bodies().front().shape.isNull());
}

void AssemblyInterfaceTest::unsavedWorkSurvivesACrash() {
    const QString folder = m_directory.filePath(QStringLiteral("recovery"));
    // An LCAD that keeps a copy of unsaved work and then crashes.
    QProcess child;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LCAD_TEST_CRASH_IN"), folder);
    child.setProcessEnvironment(environment);
    child.start(QCoreApplication::applicationFilePath(), {});
    QVERIFY(child.waitForFinished(30000));
    QCOMPARE(child.exitStatus(), QProcess::CrashExit);

    MainWindow window;
    show(window);
    window.enableRecovery(folder);
    QVERIFY(window.recovery() != nullptr);
    QCOMPARE(window.recovery()->leftBehind().size(), std::size_t{1});
    QCOMPARE(window.offerRecovery(false), 1);
    // In a tab of its own, with its changes not saved, belonging to its own file.
    QCOMPARE(window.documentCount(), 2);
    QCOMPARE(window.document()->bodies().size(), std::size_t{1});
    QCOMPARE(window.document()->bodies().front().name, std::string("Unsaved"));
    QVERIFY(window.document()->isModified());
    QCOMPARE(window.filePath(), QStringLiteral("/somewhere/part.lcad"));
    QVERIFY(window.recovery()->leftBehind().empty());

    // From now on its copy is kept in this LCAD's own folder; more work is kept too.
    window.document()->addBody("More", lcad::kernel::makeBox(1.0, 1.0, 1.0));
    window.autosave();
    const QDir session(window.recovery()->sessionFolder());
    QVERIFY(!session.entryList({QStringLiteral("*.lcad")}, QDir::Files).isEmpty());
    // Saved, its copy goes.
    QString error;
    QVERIFY(window.saveTo(m_directory.filePath(QStringLiteral("restored.lcad")), &error));
    window.autosave();
    QCOMPARE(session.entryList({QStringLiteral("*.lcad")}, QDir::Files).size(), 0);
}

void AssemblyInterfaceTest::thePullArrowIsDraggedAndTyped() {
    MainWindow window;
    show(window);
    // The planes a sketch is put on, seen through, before one is chosen.
    window.document()->addBody("Block", lcad::kernel::makeBox(30.0, 30.0, 30.0));
    QVERIFY(trigger(window, "sketch.new"));
    render(window);
    keep(window, "sketch-planes");
    window.cancelCommand();
    window.document()->undo();
    const ObjectId sketch = window.createSketch(lcad::kernel::planeXY());
    lcad::sketch::Sketch s = window.document()->findSketch(sketch)->geometry;
    lcad::sketch::addRectangle(s, {0.0, 0.0}, {40.0, 30.0});
    window.document()->setSketchGeometry(sketch, s, "Rectangle");
    render(window);
    QVERIFY(trigger(window, "solid.pull"));
    FeaturePanel* panel = window.featurePanel();
    QVERIFY(panel != nullptr);
    window.viewport()->setStandardView(lcad::viewport::StandardView::Front);
    render(window);
    QTRY_VERIFY(window.viewport()->hasDistanceHandle());
    QLineEdit* field = window.viewport()->distanceField();
    QVERIFY(field != nullptr && field->isVisible());
    window.viewport()->setStandardView(lcad::viewport::StandardView::Home);
    render(window);
    keep(window, "pull-arrow");
    window.viewport()->setStandardView(lcad::viewport::StandardView::Front);
    render(window);
    QCOMPARE(field->text(), QStringLiteral("10 mm"));

    // Typed: negative goes the other way.
    field->setFocus();
    field->selectAll();
    QTest::keyClicks(field, QStringLiteral("-12.5"));
    QTest::keyClick(field, Qt::Key_Return);
    QCOMPARE(panel->value(QStringLiteral("distance")), -12.5);
    render(window);
    QCOMPARE(field->text(), QStringLiteral("-12.5 mm"));

    // Dragged: seen from the front, up the screen is up the pull.
    render(window);
    const QPointF grip = window.viewport()->distanceHandleGrip();
    const QPoint from = grip.toPoint();
    lcad::test::movePointer(window.viewport(), from);
    QTest::mousePress(window.viewport(), Qt::LeftButton, Qt::NoModifier, from);
    // Twenty millimeters up, as the view draws them.
    const QPointF zero = window.viewport()->project(20.0, 15.0, 0.0);
    const QPointF twenty = window.viewport()->project(20.0, 15.0, 20.0);
    const QPoint to = from + (twenty - zero).toPoint();
    QMouseEvent move(QEvent::MouseMove, QPointF(to), window.viewport()->mapToGlobal(QPointF(to)),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window.viewport(), &move);
    QTest::mouseRelease(window.viewport(), Qt::LeftButton, Qt::NoModifier, to);
    const double dragged = panel->value(QStringLiteral("distance"));
    QVERIFY2(std::abs(dragged - 7.5) < 1.0, qPrintable(QString::number(dragged)));
    // Nothing was selected by the drag.
    QVERIFY(window.viewport()->selection().empty());

    // Done, the arrow goes.
    QTest::mouseClick(okButton(panel), Qt::LeftButton);
    QVERIFY(!window.viewport()->hasDistanceHandle());
    QCOMPARE(window.document()->features().size(), std::size_t{1});
}

QTEST_MAIN(AssemblyInterfaceTest)
#include "AssemblyInterfaceTest.moc"
