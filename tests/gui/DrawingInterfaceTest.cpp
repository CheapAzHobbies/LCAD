// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/Commands.h>
#include <lcad/app/DrawingWorkspace.h>
#include <lcad/app/MainWindow.h>
#include <lcad/app/Theme.h>
#include <lcad/document/Document.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/Transform.h>
#include <lcad/viewport/ViewportWidget.h>

#include <FrameChecks.h>
#include <GraphicsMessages.h>
#include <GuiHelpers.h>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QLineEdit>
#include <QRegularExpression>
#include <QSettings>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>

#include <algorithm>
#include <cmath>

using lcad::app::DrawingWorkspace;
using lcad::app::MainWindow;
using lcad::app::SheetView;
using lcad::app::Workspace;
using lcad::document::DimensionKind;
using lcad::document::Drawing;
using lcad::document::ObjectId;
using lcad::document::ViewKind;
using lcad::drawing::Point2D;
using lcad::test::keepFrame;

namespace {

// A plate of 60 by 40 by 10 with a hole of radius 5 through it.
lcad::kernel::Shape plate() {
    using namespace lcad::kernel;
    const NamedShape box = nameFaces(makeBox(60.0, 40.0, 10.0), "p");
    const NamedShape hole = nameFaces(translated(makeCylinder(5.0, 12.0), 30.0, 20.0, -1.0), "h");
    return combine(box, hole, Combination::Cut).shape;
}

} // namespace

// Drawings as the window offers them: a workspace of their own, a sheet of views of the model,
// dimensions by a click on what they measure, sections, details, notes, the title block, and PDF
// and DXF files.
class DrawingInterfaceTest : public QObject {
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

    void theWorkspaceChangesWhatTheWindowIsFor();
    void aSheetGetsDimensionsByClickingWhatTheyMeasure();
    void sectionsDetailsAndNotesAreAddedAndMoved();
    void theTitleBlockAndTheSheetAreChangedInThePanel();
    void aSheetFollowsTheModelAndUndo();
    void aDrawingIsExportedSavedAndReopened();

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
    static void click(MainWindow& window, const Point2D& at,
                      Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        SheetView* view = window.drawings()->view();
        QTest::mouseClick(view, Qt::LeftButton, modifiers, view->toWidget(at).toPoint());
    }
    // The view of a kind on the sheet.
    static const lcad::drawing::ViewBox& boxOf(const MainWindow& window, ViewKind kind) {
        const Drawing* drawing = window.drawings()->drawing();
        const auto view =
            std::find_if(drawing->views.begin(), drawing->views.end(),
                         [kind](const lcad::document::DrawingView& v) { return v.kind == kind; });
        const auto& boxes = window.drawings()->view()->sheet().views;
        return *std::find_if(boxes.begin(), boxes.end(),
                             [view](const lcad::drawing::ViewBox& b) { return b.view == view->id; });
    }

    QTemporaryDir m_directory;
    lcad::test::GraphicsMessages m_graphicsMessages;
};

void DrawingInterfaceTest::init() {
    QSettings().clear();
    lcad::app::Theme::instance().load();
    QVERIFY(m_directory.isValid());
}

void DrawingInterfaceTest::cleanup() {
    const QStringList complaints = m_graphicsMessages.take();
    QVERIFY2(complaints.isEmpty(), qPrintable(complaints.join(QStringLiteral(" | "))));
}

void DrawingInterfaceTest::theWorkspaceChangesWhatTheWindowIsFor() {
    MainWindow window;
    show(window);
    window.document()->addBody("Plate", plate());
    auto* center = window.findChild<QStackedWidget*>(QStringLiteral("centerStack"));
    auto* modeling = window.findChild<QToolBar*>(QStringLiteral("modelingToolbar"));
    auto* sheetTools = window.findChild<QToolBar*>(QStringLiteral("drawingToolbar"));
    auto* box = window.findChild<QComboBox*>(QStringLiteral("workspaceBox"));
    QVERIFY(center != nullptr && modeling != nullptr && sheetTools != nullptr && box != nullptr);
    QCOMPARE(window.workspace(), Workspace::Design);
    QCOMPARE(center->currentWidget(), window.viewport());
    QVERIFY(modeling->isVisible());
    QVERIFY(!sheetTools->isVisible());
    QVERIFY(!action(window, "drawing.dimension")->isEnabled());

    // The box at the top, as a user would.
    box->setCurrentIndex(box->findText(QStringLiteral("Drawing")));
    emit box->activated(box->currentIndex());
    QCOMPARE(window.workspace(), Workspace::Drawing);
    QCOMPARE(center->currentWidget(), window.drawings()->view());
    QTRY_VERIFY(sheetTools->isVisible());
    QVERIFY(!modeling->isVisible());
    QCOMPARE(window.inspectorPanel(), window.drawings()->panel());
    // A first look gives a sheet of the model to look at.
    QVERIFY(window.drawings()->drawing() != nullptr);
    QCOMPARE(window.drawings()->drawing()->views.size(), std::size_t{4});
    QCOMPARE(window.drawings()->view()->sheet().views.size(), std::size_t{4});
    keepFrame(window.grab().toImage(), "drawing-sheet");

    // What belongs to the model cannot be started here, and D is the sheet's Dimension.
    QVERIFY(!action(window, "solid.pull")->isEnabled());
    QVERIFY(!action(window, "sketch.new")->isEnabled());
    QVERIFY(action(window, "drawing.dimension")->isEnabled());
    QVERIFY(action(window, "edit.undo")->isEnabled());
    window.drawings()->view()->setFocus();
    QTest::keyClick(&window, Qt::Key_D);
    QCOMPARE(window.drawings()->tool(), DrawingWorkspace::Tool::Dimension);
    QVERIFY(window.drawings()->panel()->promptText().contains(QStringLiteral("line")));
    QTest::keyClick(&window, Qt::Key_Escape);
    QCOMPARE(window.drawings()->tool(), DrawingWorkspace::Tool::Select);

    QVERIFY(trigger(window, "workspace.design"));
    QCOMPARE(center->currentWidget(), window.viewport());
    QCOMPARE(box->currentText(), QStringLiteral("Design"));
    QTRY_VERIFY(modeling->isVisible());
    QVERIFY(action(window, "sketch.new")->isEnabled());
    QVERIFY(!action(window, "drawing.note")->isEnabled());

    // A sketch under way is finished on the way out.
    QVERIFY(trigger(window, "sketch.new"));
    window.cancelCommand();
}

void DrawingInterfaceTest::aSheetGetsDimensionsByClickingWhatTheyMeasure() {
    MainWindow window;
    show(window);
    window.document()->addBody("Plate", plate());
    window.setWorkspace(Workspace::Drawing);
    QTRY_VERIFY(!window.drawings()->view()->sheet().views.empty());
    window.drawings()->view()->fit();

    const auto dimensions = [&window] { return window.drawings()->view()->sheet().dimensions; };
    const lcad::drawing::ViewBox top = boxOf(window, ViewKind::Top);

    // One click on a line gives its length.
    QVERIFY(trigger(window, "drawing.dimension"));
    click(window, {(top.left + top.right) / 2.0, top.bottom});
    QCOMPARE(dimensions().size(), std::size_t{1});
    QVERIFY(qAbs(dimensions()[0].value - 60.0) < 1e-6);
    QCOMPARE(window.drawings()->drawing()->dimensions[0].kind, DimensionKind::Horizontal);
    // And it goes outside the view, not across it.
    QVERIFY(window.drawings()->view()->sheet().filled.size() >= 2);

    // An upright one too, from the same tool.
    click(window, {top.right, (top.bottom + top.top) / 2.0});
    QCOMPARE(dimensions().size(), std::size_t{2});
    QVERIFY(qAbs(dimensions()[1].value - 40.0) < 1e-6);
    QCOMPARE(window.drawings()->drawing()->dimensions[1].kind, DimensionKind::Vertical);

    // A circle by its diameter; with Shift, by its radius.
    const auto circle = std::find_if(
        window.drawings()->view()->sheet().snapCircles.begin(),
        window.drawings()->view()->sheet().snapCircles.end(),
        [&top](const lcad::drawing::SnapCircle& c) { return c.view == top.view; });
    QVERIFY(circle != window.drawings()->view()->sheet().snapCircles.end());
    const Point2D rim{circle->center.x + circle->radius, circle->center.y};
    const Point2D center = circle->center;
    const double radius = circle->radius;
    click(window, rim);
    QCOMPARE(dimensions().size(), std::size_t{3});
    QCOMPARE(QString::fromStdString(dimensions()[2].text), QStringLiteral("Ø10"));
    click(window, {center.x - radius, center.y}, Qt::ShiftModifier);
    QCOMPARE(dimensions().size(), std::size_t{4});
    QCOMPARE(QString::fromStdString(dimensions()[3].text), QStringLiteral("R5"));

    // Two points: a mark on the first, and the prompt moves on.
    click(window, {top.left, top.bottom});
    QVERIFY(window.drawings()->panel()->promptText().contains(QStringLiteral("second")));
    click(window, {top.right, top.top});
    QCOMPARE(dimensions().size(), std::size_t{5});
    QVERIFY(qAbs(dimensions()[4].value - std::hypot(60.0, 40.0)) < 1e-6);
    keepFrame(window.grab().toImage(), "drawing-dimensions");

    // Selected in the Select tool, its panel shows it; its text gets a tolerance there.
    QTest::keyClick(&window, Qt::Key_Escape);
    window.drawings()->view()->setFocus();
    click(window, dimensions()[0].textAt);
    QCOMPARE(window.drawings()->view()->selected().kind, SheetView::Selected::Kind::Dimension);
    QLineEdit* suffix = window.drawings()->panel()->suffixField();
    QVERIFY(suffix->isVisible());
    suffix->setText(QStringLiteral(" ±0.1"));
    emit suffix->editingFinished();
    QCOMPARE(QString::fromStdString(dimensions()[0].text), QStringLiteral("60 ±0.1"));

    // And Delete removes it, one undo step.
    window.drawings()->view()->setFocus();
    QVERIFY(trigger(window, "edit.delete"));
    QCOMPARE(dimensions().size(), std::size_t{4});
    QVERIFY(trigger(window, "edit.undo"));
    QCOMPARE(dimensions().size(), std::size_t{5});
}

void DrawingInterfaceTest::sectionsDetailsAndNotesAreAddedAndMoved() {
    MainWindow window;
    show(window);
    window.document()->addBody("Plate", plate());
    window.setWorkspace(Workspace::Drawing);
    QTRY_VERIFY(!window.drawings()->view()->sheet().views.empty());
    window.drawings()->view()->fit();
    SheetView* view = window.drawings()->view();
    const auto kinds = [&window](ViewKind kind) {
        const auto& views = window.drawings()->drawing()->views;
        return std::count_if(views.begin(), views.end(),
                             [kind](const auto& each) { return each.kind == kind; });
    };

    // A section through the hole, across the view from above.
    lcad::drawing::ViewBox top = boxOf(window, ViewKind::Top);
    QVERIFY(trigger(window, "drawing.sectionAcross"));
    click(window, {(top.left + top.right) / 2.0, (top.bottom + top.top) / 2.0});
    QCOMPARE(kinds(ViewKind::Section), 1);
    QCOMPARE(window.drawings()->tool(), DrawingWorkspace::Tool::Select);
    QVERIFY2(view->sheet().problems.empty(),
             qPrintable(QString::fromStdString(view->sheet().problems.empty()
                                                   ? std::string()
                                                   : view->sheet().problems.front())));
    QVERIFY(std::any_of(view->sheet().labels.begin(), view->sheet().labels.end(),
                        [](const lcad::drawing::Label& label) {
                            return label.text == "SECTION A-A";
                        }));
    QVERIFY(std::any_of(view->sheet().strokes.begin(), view->sheet().strokes.end(),
                        [](const lcad::drawing::Stroke& s) {
                            return s.pen == lcad::drawing::Pen::Hatch;
                        }));

    // A detail of its corner, which goes by the next letter.
    QVERIFY(trigger(window, "drawing.detail"));
    click(window, {top.right, top.top});
    QCOMPARE(kinds(ViewKind::Detail), 1);
    QVERIFY(std::any_of(view->sheet().labels.begin(), view->sheet().labels.end(),
                        [](const lcad::drawing::Label& label) {
                            return label.text.rfind("DETAIL B", 0) == 0;
                        }));

    // A note: click where, type, Enter.
    QVERIFY(trigger(window, "drawing.note"));
    click(window, {40.0, 60.0});
    QVERIFY(view->textField() != nullptr);
    QTest::keyClicks(view->textField(), QStringLiteral("Break all edges"));
    QTest::keyClick(view->textField(), Qt::Key_Return);
    QTRY_VERIFY(view->textField() == nullptr);
    QCOMPARE(window.drawings()->drawing()->notes.size(), std::size_t{1});
    QVERIFY(std::any_of(view->sheet().labels.begin(), view->sheet().labels.end(),
                        [](const lcad::drawing::Label& label) {
                            return label.text == "Break all edges";
                        }));
    keepFrame(window.grab().toImage(), "drawing-section-detail");

    // A view is dragged to another place, one undo step, with what it shows.
    const lcad::drawing::ViewBox iso = boxOf(window, ViewKind::Isometric);
    const QPoint from = view->toWidget({iso.x, iso.y}).toPoint();
    const QPoint to = from + QPoint(-40, 30);
    QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, from);
    QMouseEvent move(QEvent::MouseMove, to, view->mapToGlobal(to), Qt::NoButton, Qt::LeftButton,
                     Qt::NoModifier);
    QApplication::sendEvent(view, &move);
    QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, to);
    const lcad::drawing::ViewBox moved = boxOf(window, ViewKind::Isometric);
    QVERIFY(moved.x < iso.x - 1.0);
    QVERIFY(moved.y < iso.y - 1.0);
    QVERIFY(action(window, "edit.undo")->text().contains(QStringLiteral("Move View")));
    QVERIFY(trigger(window, "edit.undo"));
    QVERIFY(qAbs(boxOf(window, ViewKind::Isometric).x - iso.x) < 1e-6);

    // A view goes with what is taken from it.
    click(window, {top.x, top.y});
    QCOMPARE(view->selected().kind, SheetView::Selected::Kind::View);
    QVERIFY(window.drawings()->panel()->hiddenBox()->isVisible());
    QVERIFY(trigger(window, "edit.delete"));
    QCOMPARE(kinds(ViewKind::Top), 0);
    QCOMPARE(kinds(ViewKind::Section), 0);
    QCOMPARE(kinds(ViewKind::Detail), 0);
    QVERIFY(trigger(window, "edit.undo"));
    QCOMPARE(kinds(ViewKind::Detail), 1);

    // Added from the menu, a view lands on the sheet.
    const std::size_t before = window.drawings()->drawing()->views.size();
    QVERIFY(trigger(window, "drawing.view.back"));
    QCOMPARE(window.drawings()->drawing()->views.size(), before + 1);
    QCOMPARE(view->selected().kind, SheetView::Selected::Kind::View);
}

void DrawingInterfaceTest::theTitleBlockAndTheSheetAreChangedInThePanel() {
    MainWindow window;
    show(window);
    window.document()->addBody("Plate", plate());
    window.setWorkspace(Workspace::Drawing);
    QTRY_VERIFY(!window.drawings()->view()->sheet().views.empty());
    lcad::app::DrawingPanel* panel = window.drawings()->panel();

    panel->titleField()->setText(QStringLiteral("Mounting plate"));
    emit panel->titleField()->editingFinished();
    panel->partNumberField()->setText(QStringLiteral("LC-0042"));
    emit panel->partNumberField()->editingFinished();
    const auto& labels = window.drawings()->view()->sheet().labels;
    QVERIFY(std::any_of(labels.begin(), labels.end(), [](const lcad::drawing::Label& label) {
        return label.text == "Mounting plate";
    }));
    QVERIFY(action(window, "edit.undo")->text().contains(QStringLiteral("Part Number")));

    // Another sheet size.
    panel->sizeBox()->setCurrentIndex(panel->sizeBox()->findText(QStringLiteral("A2")));
    emit panel->sizeBox()->activated(panel->sizeBox()->currentIndex());
    QCOMPARE(window.drawings()->view()->sheet().width, 594.0);
    QCOMPARE(window.drawings()->drawing()->size, lcad::document::SheetSize::A2);

    // The standard: and a second drawing beside the first, chosen in the panel.
    panel->standardBox()->setCurrentIndex(1);
    emit panel->standardBox()->activated(1);
    QCOMPARE(window.drawings()->drawing()->standard, lcad::document::DrawingStandard::Ansi);
    QVERIFY(trigger(window, "drawing.new"));
    QCOMPARE(window.document()->drawings().size(), std::size_t{2});
    QCOMPARE(panel->drawingBox()->count(), 2);
    QCOMPARE(window.drawings()->drawing()->name, std::string("Drawing 2"));
    emit panel->drawingChosen(window.document()->drawings().front().id);
    QCOMPARE(window.drawings()->drawing()->name, std::string("Drawing 1"));
    QVERIFY(trigger(window, "drawing.delete"));
    QCOMPARE(window.document()->drawings().size(), std::size_t{1});
    QCOMPARE(window.drawings()->drawing()->name, std::string("Drawing 2"));
}

void DrawingInterfaceTest::aSheetFollowsTheModelAndUndo() {
    MainWindow window;
    show(window);
    const ObjectId body = window.document()->addBody("Plate", plate());
    window.setWorkspace(Workspace::Drawing);
    QTRY_VERIFY(!window.drawings()->view()->sheet().views.empty());
    const auto width = [&window] {
        const lcad::drawing::ViewBox box = boxOf(window, ViewKind::Front);
        return box.right - box.left;
    };
    const double before = width();

    // Another body beside the first widens every view of the whole model.
    const ObjectId second =
        window.document()->addBody("Block", lcad::kernel::makeBox(200.0, 40.0, 10.0));
    QVERIFY(width() > before);
    // Hidden, it is left out again.
    window.document()->setBodyVisible(second, false);
    QVERIFY(qAbs(width() - before) < 1e-6);

    // With nothing to show, the panel says so.
    window.document()->setBodyVisible(body, false);
    QVERIFY(window.drawings()->panel()->problemsText().contains(QStringLiteral("no body")));
}

void DrawingInterfaceTest::aDrawingIsExportedSavedAndReopened() {
    const QString folder = m_directory.path();
    {
        MainWindow window;
        show(window);
        window.document()->addBody("Plate", plate());
        window.setWorkspace(Workspace::Drawing);
        QTRY_VERIFY(!window.drawings()->view()->sheet().views.empty());
        window.drawings()->view()->fit();
        const lcad::drawing::ViewBox top = boxOf(window, ViewKind::Top);
        QVERIFY(trigger(window, "drawing.dimension"));
        click(window, {(top.left + top.right) / 2.0, top.bottom});

        QString error;
        const QString pdf = folder + QStringLiteral("/sheet.pdf");
        QVERIFY2(window.drawings()->exportTo(pdf, &error), qPrintable(error));
        QFile pdfFile(pdf);
        QVERIFY(pdfFile.open(QIODevice::ReadOnly));
        const QByteArray bytes = pdfFile.readAll();
        QVERIFY(bytes.startsWith("%PDF"));
        QVERIFY(bytes.size() > 2000);
        // One page, of the sheet's size: 420 by 297 millimeters in points.
        const QRegularExpression mediaBox(QStringLiteral("/MediaBox \\[0 0 ([0-9.]+) ([0-9.]+)\\]"));
        const QRegularExpressionMatch page = mediaBox.match(QString::fromLatin1(bytes));
        QVERIFY(page.hasMatch());
        QVERIFY(qAbs(page.captured(1).toDouble() - 420.0 / 25.4 * 72.0) < 2.0);
        QVERIFY(qAbs(page.captured(2).toDouble() - 297.0 / 25.4 * 72.0) < 2.0);

        const QString dxf = folder + QStringLiteral("/sheet.dxf");
        QVERIFY2(window.drawings()->exportTo(dxf, &error), qPrintable(error));
        QFile dxfFile(dxf);
        QVERIFY(dxfFile.open(QIODevice::ReadOnly));
        const QByteArray text = dxfFile.readAll();
        QVERIFY(text.contains("ENTITIES"));
        QVERIFY(text.contains("VISIBLE"));

        QVERIFY(!window.drawings()->exportTo(folder + QStringLiteral("/no/such/sheet.dxf"), &error));
        QVERIFY(!error.isEmpty());

        QVERIFY(window.saveTo(folder + QStringLiteral("/part.lcad"), &error));
    }
    MainWindow window;
    show(window);
    QString error;
    QVERIFY(window.loadFrom(folder + QStringLiteral("/part.lcad"), &error));
    QCOMPARE(window.document()->drawings().size(), std::size_t{1});
    window.setWorkspace(Workspace::Drawing);
    QTRY_COMPARE(window.drawings()->view()->sheet().dimensions.size(), std::size_t{1});
    QVERIFY(qAbs(window.drawings()->view()->sheet().dimensions[0].value - 60.0) < 1e-6);

    // Another document has its own drawings, or none.
    window.newDocument();
    QVERIFY(window.drawings()->drawing() == nullptr);
    QCOMPARE(window.drawings()->view()->sheet().views.size(), std::size_t{0});
}

QTEST_MAIN(DrawingInterfaceTest)
#include "DrawingInterfaceTest.moc"
