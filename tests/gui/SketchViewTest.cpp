// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/document/Document.h>
#include <lcad/kernel/Plane.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/Transform.h>
#include <lcad/sketch/Edit.h>
#include <lcad/viewport/SketchEditor.h>
#include <lcad/viewport/ViewportWidget.h>

#include <FrameChecks.h>
#include <GraphicsMessages.h>

#include <QApplication>
#include <QLineEdit>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>
#include <QWheelEvent>

#include <cmath>
#include <memory>

using lcad::document::Document;
using lcad::document::ObjectId;
using lcad::sketch::Constraint;
using lcad::sketch::ConstraintId;
using lcad::sketch::ConstraintKind;
using lcad::sketch::Entity;
using lcad::sketch::EntityId;
using lcad::sketch::EntityKind;
using lcad::sketch::Point2;
using lcad::sketch::Sketch;
using lcad::test::isNear;
using lcad::test::keepFrame;
using lcad::viewport::SelectedItem;
using lcad::viewport::SelectionFilter;
using lcad::viewport::SketchEditor;
using lcad::viewport::SketchTool;
using lcad::viewport::StandardView;
using lcad::viewport::ViewportWidget;

namespace {

bool sameDirection(const QVector3D& a, const QVector3D& b) {
    return (a - b).length() < 1e-3F;
}

double apart(Point2 a, Point2 b) {
    return std::hypot(b.x - a.x, b.y - a.y);
}

} // namespace

// The sketch editor inside the 3D view, driven the way a user drives it: with the pointer and
// the keyboard. What a tool made is always checked in the document, by measuring the sketch;
// pictures are only used to establish that something was drawn, and in which color.
class SketchViewTest : public QObject {
    Q_OBJECT

public:
    static void initMain() { lcad::viewport::prepareApplication(); }

private slots:
    void init();
    void cleanup();

    // Sketches among the bodies
    void sketchesAreDrawnAndFollowTheDocument();
    void aSketchIsPickedAsAWhole();

    // Choosing a plane
    void aPlaneIsChosenByClickingItInTheView();
    void planesCanBeChosenThroughABodyAtTheOrigin();

    // Opening and closing
    void editingTurnsTheCameraToFaceThePlane_data();
    void editingTurnsTheCameraToFaceThePlane();
    void finishingPutsTheCameraBack();
    void pointerPositionsMapToTheSketchAndBack();
    void bodiesCannotBePickedWhileSketching();
    void theMiddleButtonStillMovesTheCamera();
    void undoingTheSketchClosesIt();

    // Drawing
    void lineToolDrawsJoinedLines();
    void anOutlineClosesOnItsFirstPoint();
    void nearlyLevelLinesBecomeHorizontalUnlessInferenceIsOff();
    void endsLandOnExistingGeometryAndStayThere();
    void rectangleToolDrawsAConstrainedRectangle();
    void circleArcAndPointTools();
    void ellipseAndSplineTools();
    void polygonAndSlotTools();
    void constructionModeDrawsConstructionGeometry();
    void typingWhileDrawingGivesTheSize();

    // Constraints and dimensions
    void constraintsApplyToTheSelection();
    void aConstraintWithoutAFittingSelectionBecomesTheTool();
    void redundantAndConflictingConstraintsAreRefused();
    void aFullyConstrainedRectangleFollowsItsDimensions();
    void aLineIsDimensionedByClickingIt();
    void anImpossibleOrMeaninglessValueIsKeptForCorrection();
    void markersAndValuesCanBePickedAndDeleted();

    // Selecting and moving
    void clicksAndABoxSelect();
    void draggingMovesFreeGeometryAsOneUndoStep();
    void deleteAndConstructionActOnTheSelection();

    // Modifying
    void trimAndExtendActOnTheCurveClicked();
    void offsetAndMirror();
    void projectBringsEdgesOfABodyIntoTheSketch();
    void aDimensionIsHorizontalOrVerticalByWhereItIsPut();

    // Drawing on screen
    void geometryIsColoredByItsState();

private:
    void render() { m_viewport->grabFramebuffer(); }
    [[nodiscard]] SketchEditor* editor() const { return m_viewport->sketchEditor(); }
    [[nodiscard]] const Sketch& sketch() const {
        return m_document->findSketch(m_sketch)->geometry;
    }
    // Where a position of the sketch is in the widget.
    [[nodiscard]] QPoint at(double u, double v) const {
        return editor()->toWidget({u, v}).toPoint();
    }
    // A place in the view with nothing in it: clear of the origin, the view cube, and the axes.
    [[nodiscard]] QPoint emptyCorner() const { return {40, 40}; }
    void move(const QPoint& point);
    void click(const QPoint& point, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    void drag(const QPoint& from, const QPoint& to);
    void type(const QString& text);
    // Opens a new sketch on the XY plane with a view that shows about -20..120 by -20..80.
    void openSketch();
    // The curves of the sketch, in the order they were made.
    [[nodiscard]] std::vector<Entity> curves(EntityKind kind) const;
    [[nodiscard]] int count(ConstraintKind kind) const;
    [[nodiscard]] Point2 startOf(EntityId curve) const {
        return sketch().position(sketch().find(curve)->start);
    }
    [[nodiscard]] Point2 endOf(EntityId curve) const {
        return sketch().position(sketch().find(curve)->end);
    }
    // What the editor drew, on a clear background, with colors as they were painted.
    // The middle of the patch of an origin plane while a plane is being chosen. A patch is 120
    // pixels along each of its axes as seen square on, and reaches from the origin toward the
    // side the view is looked at from.
    [[nodiscard]] QPoint onPlane(lcad::viewport::OriginItem plane) const {
        using lcad::viewport::OriginItem;
        const QVector3D looking = m_viewport->viewDirection();
        const auto toward = [&looking](const QVector3D& axis) {
            return QVector3D::dotProduct(looking, axis) > 1e-6F ? -axis : axis;
        };
        const QVector3D u =
            toward(plane == OriginItem::PlaneYZ ? QVector3D(0, 1, 0) : QVector3D(1, 0, 0));
        const QVector3D v =
            toward(plane == OriginItem::PlaneXY ? QVector3D(0, 1, 0) : QVector3D(0, 0, 1));
        const double pixelsPerMillimeter = m_viewport->height() / m_viewport->cameraState().scale;
        const QPointF origin = m_viewport->project(0.0, 0.0, 0.0);
        const auto onScreen = [&](const QVector3D& axis) {
            return (m_viewport->project(axis.x(), axis.y(), axis.z()) - origin) /
                   pixelsPerMillimeter * 120.0;
        };
        return (origin + (onScreen(u) + onScreen(v)) * 0.5).toPoint();
    }
    [[nodiscard]] QImage overlayImage() const {
        return editor()->overlay()->grab().toImage().convertToFormat(QImage::Format_ARGB32);
    }

    std::unique_ptr<Document> m_document;
    ViewportWidget* m_viewport = nullptr;
    ObjectId m_sketch;
    lcad::test::GraphicsMessages m_graphicsMessages;
};

void SketchViewTest::init() {
    QTest::failOnWarning(QRegularExpression(QStringLiteral("OpenGL|QRhi|context|surface"),
                                            QRegularExpression::CaseInsensitiveOption));
    m_document = std::make_unique<Document>();
    m_viewport = new ViewportWidget;
    m_viewport->resize(800, 600);
    m_viewport->setDocument(m_document.get());
    m_viewport->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_viewport));
    m_sketch = {};
}

void SketchViewTest::cleanup() {
    delete m_viewport;
    m_viewport = nullptr;
    m_document.reset();
    const QStringList complaints = m_graphicsMessages.take();
    QVERIFY2(complaints.isEmpty(), qPrintable(complaints.join(QStringLiteral(" | "))));
}

void SketchViewTest::move(const QPoint& point) {
    QTest::mouseMove(m_viewport, point);
    render();
}

void SketchViewTest::click(const QPoint& point, Qt::KeyboardModifiers modifiers) {
    move(point);
    QTest::mouseClick(m_viewport, Qt::LeftButton, modifiers, point);
    render();
}

void SketchViewTest::drag(const QPoint& from, const QPoint& to) {
    move(from);
    QTest::mousePress(m_viewport, Qt::LeftButton, Qt::NoModifier, from);
    render();
    // QTest::mouseMove carries no button state, so build the moves by hand.
    const int steps = 6;
    for (int i = 1; i <= steps; ++i) {
        const QPoint point = from + (to - from) * i / steps;
        QMouseEvent event(QEvent::MouseMove, point, m_viewport->mapToGlobal(point), Qt::NoButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(m_viewport, &event);
        render();
    }
    QTest::mouseRelease(m_viewport, Qt::LeftButton, Qt::NoModifier, to);
    render();
}

void SketchViewTest::type(const QString& text) {
    // To whatever has the keyboard: the view, or the field a value is typed into.
    QWidget* target = editor()->valueField() != nullptr
                          ? static_cast<QWidget*>(editor()->valueField())
                          : static_cast<QWidget*>(m_viewport);
    QTest::keyClicks(target, text);
}

void SketchViewTest::openSketch() {
    m_sketch = m_document->addSketch("Sketch 1", lcad::kernel::planeXY());
    QVERIFY(m_viewport->editSketch(m_sketch));
    render();
    // A known view: 200 mm tall, so 3 pixels to the millimeter in a 600 pixel high widget.
    lcad::viewport::CameraState camera = m_viewport->cameraState();
    camera.center = {50.0, 30.0, 0.0};
    camera.eye = {50.0, 30.0, 500.0};
    camera.up = {0.0, 1.0, 0.0};
    camera.scale = 200.0;
    m_viewport->setCameraState(camera);
    render();
}

std::vector<Entity> SketchViewTest::curves(EntityKind kind) const {
    std::vector<Entity> found;
    for (const Entity& entity : sketch().entities()) {
        if (entity.kind == kind && (kind != EntityKind::Point || entity.owner == 0) &&
            entity.id != sketch().origin()) {
            found.push_back(entity);
        }
    }
    return found;
}

int SketchViewTest::count(ConstraintKind kind) const {
    int found = 0;
    for (const Constraint& constraint : sketch().constraints()) {
        found += constraint.kind == kind ? 1 : 0;
    }
    return found;
}

// ---------------------------------------------------------------------------------------------
// Sketches among the bodies

void SketchViewTest::sketchesAreDrawnAndFollowTheDocument() {
    const QImage empty = m_viewport->grabFramebuffer();
    const ObjectId id = m_document->addSketch("Sketch 1", lcad::kernel::planeXY());
    Sketch geometry;
    lcad::sketch::addRectangle(geometry, {0.0, 0.0}, {80.0, 50.0});
    m_document->setSketchGeometry(id, geometry, "Rectangle");
    m_viewport->setStandardView(StandardView::Top);
    const QImage drawn = m_viewport->grabFramebuffer();
    keepFrame(drawn, "sketch-among-bodies");

    // Its lines are there, in the color sketches have.
    const QColor color = m_viewport->colors().sketch;
    const auto hasSketchColor = [&](const QImage& frame, const QPointF& where) {
        const QPoint pixel = (where * frame.devicePixelRatio()).toPoint();
        for (int dy = -3; dy <= 3; ++dy) {
            for (int dx = -3; dx <= 3; ++dx) {
                if (frame.rect().contains(pixel + QPoint(dx, dy)) &&
                    isNear(frame.pixel(pixel + QPoint(dx, dy)), color, 40)) {
                    return true;
                }
            }
        }
        return false;
    };
    QVERIFY(hasSketchColor(drawn, m_viewport->project(40.0, 0.0, 0.0)));
    QVERIFY(hasSketchColor(drawn, m_viewport->project(80.0, 25.0, 0.0)));
    QVERIFY(!hasSketchColor(drawn, m_viewport->project(40.0, 25.0, 0.0)));

    // It follows the document: a changed sketch is drawn changed, a hidden one not at all.
    lcad::sketch::addRectangle(geometry, {20.0, 20.0}, {60.0, 30.0});
    m_document->setSketchGeometry(id, geometry, "Rectangle");
    QVERIFY(hasSketchColor(m_viewport->grabFramebuffer(), m_viewport->project(40.0, 20.0, 0.0)));
    m_document->undo();
    QVERIFY(!hasSketchColor(m_viewport->grabFramebuffer(), m_viewport->project(40.0, 20.0, 0.0)));
    QVERIFY(hasSketchColor(m_viewport->grabFramebuffer(), m_viewport->project(40.0, 0.0, 0.0)));

    m_document->setSketchVisible(id, false);
    QVERIFY(!hasSketchColor(m_viewport->grabFramebuffer(), m_viewport->project(40.0, 0.0, 0.0)));
    m_document->setSketchVisible(id, true);
    QVERIFY(hasSketchColor(m_viewport->grabFramebuffer(), m_viewport->project(40.0, 0.0, 0.0)));
    m_document->removeSketch(id);
    QVERIFY(!hasSketchColor(m_viewport->grabFramebuffer(), m_viewport->project(40.0, 0.0, 0.0)));
    Q_UNUSED(empty)
}

void SketchViewTest::aSketchIsPickedAsAWhole() {
    const ObjectId id = m_document->addSketch("Sketch 1", lcad::kernel::planeXY());
    Sketch geometry;
    lcad::sketch::addRectangle(geometry, {0.0, 0.0}, {80.0, 50.0});
    m_document->setSketchGeometry(id, geometry, "Rectangle");
    m_viewport->setStandardView(StandardView::Top);
    render();
    const QPoint onALine = m_viewport->project(40.0, 0.0, 0.0).toPoint();

    QSignalSpy changed(m_viewport, &ViewportWidget::selectionChanged);
    click(onALine);
    QCOMPARE(m_viewport->selection(),
             (std::vector<SelectedItem>{{id, SelectedItem::Kind::Sketch, -1}}));
    QCOMPARE(changed.count(), 1);
    // Inside the rectangle there is nothing to pick: a sketch is its curves.
    click(m_viewport->project(40.0, 25.0, 0.0).toPoint());
    QVERIFY(m_viewport->selection().empty());

    // From outside, as the browser does.
    m_viewport->setSelection({{id, SelectedItem::Kind::Sketch, -1}});
    QCOMPARE(m_viewport->selection().size(), std::size_t{1});
    m_viewport->clearSelection();

    // A filter that leaves out edges leaves out sketches.
    m_viewport->setSelectionFilter(SelectionFilter::Faces);
    click(onALine);
    QVERIFY(m_viewport->selection().empty());
    m_viewport->setSelectionFilter(SelectionFilter::Edges);
    click(onALine);
    QCOMPARE(m_viewport->selection().size(), std::size_t{1});

    // A double-click says "open this".
    QSignalSpy activated(m_viewport, &ViewportWidget::itemActivated);
    QTest::mouseDClick(m_viewport, Qt::LeftButton, Qt::NoModifier, onALine);
    QCOMPARE(activated.count(), 1);
    QCOMPARE(activated.first().first().value<SelectedItem>().body, id);
}

// ---------------------------------------------------------------------------------------------
// Choosing a plane

void SketchViewTest::aPlaneIsChosenByClickingItInTheView() {
    using lcad::viewport::OriginItem;
    m_viewport->setStandardView(StandardView::Home);
    render();
    const QImage without = m_viewport->grabFramebuffer();
    QVERIFY(!m_viewport->isPlanePicking());
    QVERIFY(!m_viewport->pickPlaneAt(onPlane(OriginItem::PlaneXY)).has_value());

    m_viewport->setPlanePicking(true);
    QVERIFY(m_viewport->isPlanePicking());
    render();
    const QImage with = m_viewport->grabFramebuffer();
    keepFrame(with, "sketch-plane-picking");
    // The three planes are there to see: each patch in its own color, where there was nothing.
    const auto tinted = [&](OriginItem plane) {
        const QPoint pixel = onPlane(plane) * with.devicePixelRatio();
        const QColor before = without.pixelColor(pixel);
        const QColor now = with.pixelColor(pixel);
        const QColor ink = ViewportWidget::originColor(plane);
        // Nearer to the plane's color than the bare background was, in every channel that
        // differs.
        const auto distance = [&ink](const QColor& c) {
            return qAbs(c.red() - ink.red()) + qAbs(c.green() - ink.green()) +
                   qAbs(c.blue() - ink.blue());
        };
        return distance(now) < distance(before) - 20;
    };
    for (const OriginItem plane : {OriginItem::PlaneXY, OriginItem::PlaneXZ, OriginItem::PlaneYZ}) {
        QVERIFY2(tinted(plane), qPrintable(QString::number(static_cast<int>(plane))));
        // And each is where a click finds it.
        const auto found = m_viewport->pickPlaneAt(onPlane(plane));
        QVERIFY(found.has_value());
        QCOMPARE(static_cast<int>(*found), static_cast<int>(plane));
    }
    QVERIFY(!m_viewport->pickPlaneAt(emptyCorner()).has_value());

    // The one under the pointer lights up, and says which it is.
    QSignalSpy hovered(m_viewport, &ViewportWidget::planeHovered);
    const QPoint onXZ = onPlane(OriginItem::PlaneXZ);
    const QColor resting = with.pixelColor(onXZ * with.devicePixelRatio());
    move(onXZ);
    render();
    QTRY_COMPARE(hovered.count(), 1);
    QCOMPARE(hovered.last().first().toInt(), static_cast<int>(OriginItem::PlaneXZ));
    const QImage lit = m_viewport->grabFramebuffer();
    keepFrame(lit, "sketch-plane-hovered");
    const QColor glowing = lit.pixelColor(onXZ * lit.devicePixelRatio());
    QVERIFY2(qAbs(glowing.green() - resting.green()) + qAbs(glowing.red() - resting.red()) +
                     qAbs(glowing.blue() - resting.blue()) >
                 25,
             qPrintable(QStringLiteral("%1 %2").arg(resting.name(), glowing.name())));
    move(emptyCorner());
    render();
    QTRY_COMPARE(hovered.count(), 2);
    QCOMPARE(hovered.last().first().toInt(), -1);

    // A click on a patch chooses that plane, and selects nothing.
    QSignalSpy picked(m_viewport, &ViewportWidget::planePicked);
    QSignalSpy selected(m_viewport, &ViewportWidget::selectionChanged);
    click(onPlane(OriginItem::PlaneYZ));
    QCOMPARE(picked.count(), 1);
    QCOMPARE(static_cast<int>(picked.first().first().value<OriginItem>()),
             static_cast<int>(OriginItem::PlaneYZ));
    QCOMPARE(selected.count(), 0);
    QVERIFY(m_viewport->selection().empty());
    // A click beside the patches chooses none.
    click(emptyCorner());
    QCOMPARE(picked.count(), 1);
    // Nor does a press on one patch that is let go somewhere else.
    QTest::mousePress(m_viewport, Qt::LeftButton, Qt::NoModifier, onPlane(OriginItem::PlaneXY));
    render();
    QTest::mouseRelease(m_viewport, Qt::LeftButton, Qt::NoModifier, emptyCorner());
    render();
    QCOMPARE(picked.count(), 1);
    // Or on another patch: which of the two was meant?
    QTest::mousePress(m_viewport, Qt::LeftButton, Qt::NoModifier, onPlane(OriginItem::PlaneXY));
    render();
    QTest::mouseRelease(m_viewport, Qt::LeftButton, Qt::NoModifier, onPlane(OriginItem::PlaneXZ));
    render();
    QCOMPARE(picked.count(), 1);
    QVERIFY(m_viewport->selection().empty());

    // The patches keep their size on screen when the view is zoomed.
    const QPoint spot = onPlane(OriginItem::PlaneXY);
    QWheelEvent wheel(spot, m_viewport->mapToGlobal(spot), QPoint(), QPoint(0, 360), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(m_viewport, &wheel);
    render();
    QVERIFY(m_viewport->pickPlaneAt(onPlane(OriginItem::PlaneXY)).has_value());
    QVERIFY(m_viewport->pickPlaneAt(onPlane(OriginItem::PlaneYZ)).has_value());

    // Switched off, they are gone, and nothing is left to click.
    m_viewport->setPlanePicking(false);
    render();
    QVERIFY(!m_viewport->pickPlaneAt(onPlane(OriginItem::PlaneXY)).has_value());
    click(onPlane(OriginItem::PlaneXY));
    QCOMPARE(picked.count(), 1);
}

void SketchViewTest::planesCanBeChosenThroughABodyAtTheOrigin() {
    using lcad::viewport::OriginItem;
    // A cube with the origin at its very middle: the planes' patches are all inside it.
    m_document->addBody("Cube", lcad::kernel::translated(lcad::kernel::makeBox(100.0, 100.0, 100.0),
                                                         -50.0, -50.0, -50.0));
    m_viewport->setStandardView(StandardView::Home);
    render();
    m_viewport->setPlanePicking(true);
    render();
    keepFrame(m_viewport->grabFramebuffer(), "sketch-plane-picking-box");

    // They are drawn over the cube all the same, and a click on one chooses the plane, not
    // the face of the cube in front of it.
    QSignalSpy picked(m_viewport, &ViewportWidget::planePicked);
    for (const OriginItem plane : {OriginItem::PlaneXY, OriginItem::PlaneXZ, OriginItem::PlaneYZ}) {
        const auto found = m_viewport->pickPlaneAt(onPlane(plane));
        QVERIFY2(found.has_value(), qPrintable(QString::number(static_cast<int>(plane))));
        QCOMPARE(static_cast<int>(*found), static_cast<int>(plane));
    }
    click(onPlane(OriginItem::PlaneXY));
    QCOMPARE(picked.count(), 1);
    QCOMPARE(static_cast<int>(picked.first().first().value<OriginItem>()),
             static_cast<int>(OriginItem::PlaneXY));
    QVERIFY(m_viewport->selection().empty());

    // The rest of the cube can still be clicked: a flat face is a place for a sketch too.
    click(m_viewport->project(-40.0, 40.0, 50.0).toPoint());
    QCOMPARE(picked.count(), 1);
    QCOMPARE(m_viewport->selection().size(), std::size_t{1});
    QCOMPARE(m_viewport->selection()[0].kind, SelectedItem::Kind::Face);
}

// ---------------------------------------------------------------------------------------------
// Opening and closing

void SketchViewTest::editingTurnsTheCameraToFaceThePlane_data() {
    QTest::addColumn<int>("which");
    QTest::addColumn<QVector3D>("direction");
    QTest::newRow("XY, from above") << 0 << QVector3D(0.0F, 0.0F, -1.0F);
    QTest::newRow("XZ, from the front") << 1 << QVector3D(0.0F, 1.0F, 0.0F);
    QTest::newRow("YZ, from the right") << 2 << QVector3D(-1.0F, 0.0F, 0.0F);
}

void SketchViewTest::editingTurnsTheCameraToFaceThePlane() {
    QFETCH(int, which);
    QFETCH(QVector3D, direction);
    const lcad::kernel::Plane plane = which == 0   ? lcad::kernel::planeXY()
                                      : which == 1 ? lcad::kernel::planeXZ()
                                                   : lcad::kernel::planeYZ();
    m_sketch = m_document->addSketch("Sketch 1", plane);
    QSignalSpy editing(m_viewport, &ViewportWidget::sketchEditingChanged);

    QVERIFY(m_viewport->editSketch(m_sketch));
    render();
    QCOMPARE(editing.count(), 1);
    QCOMPARE(m_viewport->editedSketch(), m_sketch);
    QVERIFY(editor()->isActive());
    QVERIFY(editor()->overlay()->isVisible());
    QCOMPARE(editor()->overlay()->size(), m_viewport->size());
    QVERIFY2(sameDirection(m_viewport->viewDirection(), direction),
             qPrintable(QStringLiteral("%1 %2 %3")
                            .arg(m_viewport->viewDirection().x())
                            .arg(m_viewport->viewDirection().y())
                            .arg(m_viewport->viewDirection().z())));
    // The sketch's own X runs to the right and its Y up.
    const QPointF origin = editor()->toWidget({0.0, 0.0});
    QVERIFY(editor()->toWidget({10.0, 0.0}).x() > origin.x() + 5.0);
    QVERIFY(qAbs(editor()->toWidget({10.0, 0.0}).y() - origin.y()) < 0.5);
    QVERIFY(editor()->toWidget({0.0, 10.0}).y() < origin.y() - 5.0);
    // With nothing to frame, the origin is in the middle and the view some 200 mm tall.
    QVERIFY(QLineF(origin, QPointF(400.0, 300.0)).length() < 2.0);
    QVERIFY(qAbs(editor()->toWidget({0.0, 100.0}).y() - 0.0) < 3.0);

    // There is no sketch with a made-up id.
    QVERIFY(!m_viewport->editSketch(ObjectId{9999}));
    QCOMPARE(m_viewport->editedSketch(), m_sketch);
}

void SketchViewTest::finishingPutsTheCameraBack() {
    m_document->addBody("Box", lcad::kernel::makeBox(100.0, 60.0, 40.0));
    m_sketch = m_document->addSketch("Sketch 1", lcad::kernel::planeXZ());
    m_viewport->setStandardView(StandardView::Home);
    render();
    const lcad::viewport::CameraState before = m_viewport->cameraState();
    const QVector3D home = m_viewport->viewDirection();

    QVERIFY(m_viewport->editSketch(m_sketch));
    render();
    QVERIFY(!sameDirection(m_viewport->viewDirection(), home));

    QSignalSpy editing(m_viewport, &ViewportWidget::sketchEditingChanged);
    m_viewport->finishSketch();
    render();
    QCOMPARE(editing.count(), 1);
    QVERIFY(!m_viewport->editedSketch().isValid());
    QVERIFY(!editor()->isActive());
    QVERIFY(!editor()->overlay()->isVisible());
    QVERIFY(sameDirection(m_viewport->viewDirection(), home));
    const lcad::viewport::CameraState after = m_viewport->cameraState();
    QVERIFY(qAbs(after.scale - before.scale) < 1e-6);
    for (std::size_t i = 0; i < 3; ++i) {
        QVERIFY(qAbs(after.center[i] - before.center[i]) < 1e-6);
    }
    // Finishing twice does nothing.
    m_viewport->finishSketch();
    QCOMPARE(editing.count(), 1);
}

void SketchViewTest::pointerPositionsMapToTheSketchAndBack() {
    // A plane that is square to nothing, through a point away from the origin.
    m_sketch = m_document->addSketch(
        "Sketch 1", lcad::kernel::makePlane({10.0, 20.0, 30.0}, {1.0, 1.0, 1.0}, {1.0, -1.0, 0.0}));
    QVERIFY(m_viewport->editSketch(m_sketch));
    render();
    for (const Point2 p : {Point2{0.0, 0.0}, Point2{37.5, -12.25}, Point2{-60.0, 44.0}}) {
        const auto back = editor()->toSketch(editor()->toWidget(p));
        QVERIFY(back.has_value());
        QVERIFY2(apart(*back, p) < 1e-6, qPrintable(QString::number(apart(*back, p))));
    }
    // Still true after the view is turned away from the plane.
    m_viewport->setStandardView(StandardView::Home);
    render();
    const auto back = editor()->toSketch(editor()->toWidget({25.0, 10.0}));
    QVERIFY(back.has_value());
    QVERIFY(apart(*back, {25.0, 10.0}) < 1e-6);

    // Seen edge-on, a position in the view is no one position of the sketch.
    m_viewport->finishSketch();
    const ObjectId flat = m_document->addSketch("Sketch 2", lcad::kernel::planeXY());
    QVERIFY(m_viewport->editSketch(flat));
    m_viewport->setStandardView(StandardView::Front);
    render();
    QVERIFY(!editor()->toSketch(QPointF(300.0, 200.0)).has_value());
}

void SketchViewTest::bodiesCannotBePickedWhileSketching() {
    const ObjectId box = m_document->addBody("Box", lcad::kernel::makeBox(100.0, 60.0, 40.0));
    m_sketch = m_document->addSketch("Sketch 1", lcad::kernel::planeXY());
    m_viewport->setSelectedBodies({box});
    QCOMPARE(m_viewport->selection().size(), std::size_t{1});

    QSignalSpy changed(m_viewport, &ViewportWidget::selectionChanged);
    QVERIFY(m_viewport->editSketch(m_sketch));
    render();
    // What was selected is let go, and a click on the box selects nothing.
    QVERIFY(m_viewport->selection().empty());
    QCOMPARE(changed.count(), 1);
    click(m_viewport->project(50.0, 30.0, 40.0).toPoint());
    QVERIFY(m_viewport->selection().empty());
    QVERIFY(m_viewport->itemsAt(m_viewport->project(50.0, 30.0, 40.0).toPoint()).empty());

    // Afterwards it can be picked again.
    m_viewport->finishSketch();
    m_viewport->setStandardView(StandardView::Top);
    render();
    click(m_viewport->project(50.0, 30.0, 40.0).toPoint());
    QCOMPARE(m_viewport->selection().size(), std::size_t{1});
}

void SketchViewTest::theMiddleButtonStillMovesTheCamera() {
    openSketch();
    const QPointF before = editor()->toWidget({0.0, 0.0});
    const QPoint from(400, 300);
    QTest::mousePress(m_viewport, Qt::MiddleButton, Qt::NoModifier, from);
    render();
    for (const QPoint& point : {from + QPoint(30, 20), from + QPoint(60, 40)}) {
        QMouseEvent event(QEvent::MouseMove, point, m_viewport->mapToGlobal(point), Qt::NoButton,
                          Qt::MiddleButton, Qt::NoModifier);
        QApplication::sendEvent(m_viewport, &event);
        render();
    }
    QTest::mouseRelease(m_viewport, Qt::MiddleButton, Qt::NoModifier, from + QPoint(60, 40));
    render();
    // Panned: the sketch moved with the pointer, and is still seen from straight above.
    const QPointF after = editor()->toWidget({0.0, 0.0});
    QVERIFY2(QLineF(after, before + QPointF(60.0, 40.0)).length() < 3.0,
             qPrintable(QStringLiteral("%1 %2").arg(after.x()).arg(after.y())));
    QVERIFY(sameDirection(m_viewport->viewDirection(), {0.0F, 0.0F, -1.0F}));
    QVERIFY(curves(EntityKind::Line).empty());
}

void SketchViewTest::undoingTheSketchClosesIt() {
    m_sketch = m_document->addSketch("Sketch 1", lcad::kernel::planeXY());
    QVERIFY(m_viewport->editSketch(m_sketch));
    render();
    QSignalSpy editing(m_viewport, &ViewportWidget::sketchEditingChanged);
    m_document->undo();
    render();
    QVERIFY(!m_viewport->editedSketch().isValid());
    QVERIFY(!editor()->isActive());
    QCOMPARE(editing.count(), 1);
    // And another document leaves no sketch open either.
    m_document->redo();
    QVERIFY(m_viewport->editSketch(m_sketch));
    Document other;
    m_viewport->setDocument(&other);
    QVERIFY(!editor()->isActive());
    m_viewport->setDocument(m_document.get());
}

// ---------------------------------------------------------------------------------------------
// Drawing

void SketchViewTest::lineToolDrawsJoinedLines() {
    openSketch();
    editor()->setTool(SketchTool::Line);
    QVERIFY(!editor()->hint().isEmpty());
    click(at(10.0, 10.0));
    QVERIFY(curves(EntityKind::Line).empty());
    QVERIFY(editor()->canCancel());
    click(at(60.0, 20.0));
    click(at(70.0, 55.0));

    const std::vector<Entity> lines = curves(EntityKind::Line);
    QCOMPARE(lines.size(), std::size_t{2});
    // Where they were clicked, to the pixel: a third of a millimeter in this view.
    QVERIFY(apart(startOf(lines[0].id), {10.0, 10.0}) < 0.4);
    QVERIFY(apart(endOf(lines[0].id), {60.0, 20.0}) < 0.4);
    QVERIFY(apart(endOf(lines[1].id), {70.0, 55.0}) < 0.4);
    // The second starts exactly where the first ends, and is held there.
    QVERIFY(apart(startOf(lines[1].id), endOf(lines[0].id)) < 1e-9);
    QCOMPARE(count(ConstraintKind::Coincident), 1);
    // Each line is an undo step of its own.
    QCOMPARE(QString::fromStdString(m_document->undoLabel()), QStringLiteral("Line"));

    // Esc ends the line but keeps the tool; Esc again puts the tool away.
    editor()->cancel();
    QCOMPARE(editor()->tool(), SketchTool::Line);
    move(at(20.0, 60.0));
    click(at(20.0, 60.0));
    QCOMPARE(curves(EntityKind::Line).size(), std::size_t{2});
    editor()->cancel();
    editor()->cancel();
    QCOMPARE(editor()->tool(), SketchTool::Select);
    QVERIFY(!editor()->canCancel());

    m_document->undo();
    QCOMPARE(curves(EntityKind::Line).size(), std::size_t{1});
    m_document->undo();
    QVERIFY(curves(EntityKind::Line).empty());
}

void SketchViewTest::anOutlineClosesOnItsFirstPoint() {
    openSketch();
    editor()->setTool(SketchTool::Line);
    click(at(10.0, 10.0));
    click(at(60.0, 12.0));
    click(at(35.0, 50.0));
    // Back onto the first point.
    click(at(10.0, 10.0));

    const std::vector<Entity> lines = curves(EntityKind::Line);
    QCOMPARE(lines.size(), std::size_t{3});
    QVERIFY(apart(endOf(lines[2].id), startOf(lines[0].id)) < 1e-9);
    QCOMPARE(count(ConstraintKind::Coincident), 3);
    // A closed outline, and the line is finished: the next click starts a new one.
    QCOMPARE(sketch().profiles().size(), std::size_t{1});
    QVERIFY(qAbs(sketch().profiles()[0].area - 975.0) < 40.0);
    click(at(80.0, 10.0));
    QCOMPARE(curves(EntityKind::Line).size(), std::size_t{3});
}

void SketchViewTest::nearlyLevelLinesBecomeHorizontalUnlessInferenceIsOff() {
    openSketch();
    editor()->setTool(SketchTool::Line);
    QVERIFY(editor()->hasInference());
    click(at(10.0, 10.0));
    // Two pixels off level.
    click(at(60.0, 10.0) + QPoint(0, -2));
    // And one a little off upright.
    click(at(60.0, 50.0) + QPoint(2, 0));
    editor()->cancel();

    std::vector<Entity> lines = curves(EntityKind::Line);
    QCOMPARE(lines.size(), std::size_t{2});
    QVERIFY(qAbs(endOf(lines[0].id).y - startOf(lines[0].id).y) < 1e-9);
    QVERIFY(qAbs(endOf(lines[1].id).x - startOf(lines[1].id).x) < 1e-9);
    QCOMPARE(count(ConstraintKind::Horizontal), 1);
    QCOMPARE(count(ConstraintKind::Vertical), 1);

    // Without inference a line goes exactly where it was clicked and gets no constraints.
    editor()->setInference(false);
    click(at(10.0, 40.0));
    click(at(50.0, 40.0) + QPoint(0, -2));
    editor()->cancel();
    lines = curves(EntityKind::Line);
    QCOMPARE(lines.size(), std::size_t{3});
    QVERIFY(qAbs(endOf(lines[2].id).y - startOf(lines[2].id).y) > 0.3);
    QCOMPARE(count(ConstraintKind::Horizontal), 1);
}

void SketchViewTest::endsLandOnExistingGeometryAndStayThere() {
    openSketch();
    editor()->setTool(SketchTool::Line);
    // From the origin (three pixels off it) ...
    click(at(0.0, 0.0) + QPoint(3, -2));
    click(at(50.0, 30.0));
    editor()->cancel();
    const Entity first = curves(EntityKind::Line).front();
    QVERIFY(apart(startOf(first.id), {0.0, 0.0}) < 1e-9);

    // ... a second line from the middle of the first to its end ...
    click(at(25.0, 15.0) + QPoint(2, 2));
    click(at(60.0, 5.0));
    // ... and a third from somewhere along the second back to the end of the first.
    editor()->cancel();
    click(at(80.0, 40.0));
    click(at(50.0, 30.0) + QPoint(-3, 2));
    editor()->cancel();

    const std::vector<Entity> lines = curves(EntityKind::Line);
    QCOMPARE(lines.size(), std::size_t{3});
    QVERIFY(apart(startOf(lines[1].id), {25.0, 15.0}) < 1e-9);
    QVERIFY(apart(endOf(lines[2].id), endOf(first.id)) < 1e-9);
    QCOMPARE(count(ConstraintKind::Midpoint), 1);
    // To the origin, and to the first line's end.
    QCOMPARE(count(ConstraintKind::Coincident), 2);

    // They stay there: drag the first line's end and the others follow.
    Sketch moved = sketch();
    moved.dragBy(first.end, 6.0, -4.0);
    QVERIFY(apart(moved.position(lines[2].end), moved.position(first.end)) < 1e-6);
    const Point2 middle{(moved.position(first.start).x + moved.position(first.end).x) / 2.0,
                        (moved.position(first.start).y + moved.position(first.end).y) / 2.0};
    QVERIFY(apart(moved.position(lines[1].start), middle) < 1e-6);
}

void SketchViewTest::rectangleToolDrawsAConstrainedRectangle() {
    openSketch();
    editor()->setTool(SketchTool::Rectangle);
    click(at(0.0, 0.0));
    move(at(30.0, 20.0));
    QVERIFY(curves(EntityKind::Line).empty());
    click(at(80.0, 50.0));

    const std::vector<Entity> lines = curves(EntityKind::Line);
    QCOMPARE(lines.size(), std::size_t{4});
    QCOMPARE(count(ConstraintKind::Horizontal), 2);
    QCOMPARE(count(ConstraintKind::Vertical), 2);
    // Four corners, and the first corner on the origin.
    QCOMPARE(count(ConstraintKind::Coincident), 5);
    QCOMPARE(sketch().profiles().size(), std::size_t{1});
    QVERIFY(qAbs(sketch().profiles()[0].area - 4000.0) < 60.0);
    // One undo step, and only its width and height are left to decide.
    QCOMPARE(QString::fromStdString(m_document->undoLabel()), QStringLiteral("Rectangle"));
    QCOMPARE(editor()->state().degreesOfFreedom, 2);
    // The tool is ready for another.
    QCOMPARE(editor()->tool(), SketchTool::Rectangle);
    QVERIFY(editor()->canCancel());
    m_document->undo();
    QVERIFY(curves(EntityKind::Line).empty());
}

void SketchViewTest::circleArcAndPointTools() {
    openSketch();
    editor()->setTool(SketchTool::Circle);
    click(at(30.0, 30.0));
    click(at(45.0, 30.0));
    std::vector<Entity> circles = curves(EntityKind::Circle);
    QCOMPARE(circles.size(), std::size_t{1});
    QVERIFY(apart(sketch().position(circles[0].center), {30.0, 30.0}) < 0.4);
    QVERIFY(qAbs(circles[0].radius - 15.0) < 0.4);

    // An arc by its center, its start, and its end, going counter-clockwise ...
    editor()->setTool(SketchTool::Arc);
    click(at(80.0, 30.0));
    click(at(100.0, 30.0));
    move(at(98.0, 40.0));
    move(at(90.0, 48.0));
    click(at(80.0, 50.0));
    std::vector<Entity> arcs = curves(EntityKind::Arc);
    QCOMPARE(arcs.size(), std::size_t{1});
    QVERIFY(apart(startOf(arcs[0].id), {100.0, 30.0}) < 0.4);
    QVERIFY(apart(endOf(arcs[0].id), {80.0, 50.0}) < 0.4);
    // ... is the quarter that was swept, not the other three.
    QVERIFY(lcad::sketch::distanceTo(sketch(), arcs[0].id, {80.0 + 14.142, 30.0 + 14.142}) < 0.5);
    QVERIFY(lcad::sketch::distanceTo(sketch(), arcs[0].id, {80.0 - 14.142, 30.0 - 14.142}) > 5.0);

    // Going clockwise it is still the part that was swept.
    click(at(80.0, 30.0) + QPoint(0, 180));
    click(at(100.0, 30.0) + QPoint(0, 180));
    move(at(98.0, 22.0) + QPoint(0, 180));
    move(at(90.0, 12.0) + QPoint(0, 180));
    click(at(80.0, 10.0) + QPoint(0, 180));
    arcs = curves(EntityKind::Arc);
    QCOMPARE(arcs.size(), std::size_t{2});
    // In this view 180 pixels are 60 mm: the second arc is around (80, -30).
    QVERIFY(lcad::sketch::distanceTo(sketch(), arcs[1].id, {80.0 + 14.142, -30.0 - 14.142}) < 0.5);
    QVERIFY(lcad::sketch::distanceTo(sketch(), arcs[1].id, {80.0 - 14.142, -30.0 + 14.142}) > 5.0);

    editor()->setTool(SketchTool::Point);
    click(at(10.0, 60.0));
    const std::vector<Entity> points = curves(EntityKind::Point);
    QCOMPARE(points.size(), std::size_t{1});
    QVERIFY(apart({points[0].x, points[0].y}, {10.0, 60.0}) < 0.4);
    // A point put on the circle stays on it.
    click(at(30.0, 45.0) + QPoint(0, 2));
    QCOMPARE(count(ConstraintKind::PointOnCircle), 1);
}

void SketchViewTest::ellipseAndSplineTools() {
    openSketch();
    // An ellipse: its center, the end of its major axis, and how far out it reaches across.
    editor()->setTool(SketchTool::Ellipse);
    QVERIFY(editor()->hint().contains(QStringLiteral("center")));
    click(at(40.0, 30.0));
    click(at(70.0, 30.0));
    move(at(50.0, 40.0));
    render();
    keepFrame(overlayImage(), "sketch-ellipse-preview");
    click(at(50.0, 42.0));
    const std::vector<Entity> ellipses = curves(EntityKind::Ellipse);
    QCOMPARE(ellipses.size(), std::size_t{1});
    QVERIFY(apart(sketch().position(ellipses[0].center), {40.0, 30.0}) < 0.4);
    QVERIFY(apart(sketch().position(ellipses[0].start), {70.0, 30.0}) < 0.4);
    QVERIFY(qAbs(ellipses[0].radius - 12.0) < 0.4);
    QCOMPARE(QString::fromStdString(m_document->undoLabel()), QStringLiteral("Ellipse"));
    // It is a closed outline.
    QCOMPARE(sketch().profiles().size(), std::size_t{1});
    // Clicked farther out than the major axis, it is refused with the reason.
    click(at(40.0, -40.0));
    click(at(50.0, -40.0));
    click(at(45.0, -10.0));
    QCOMPARE(curves(EntityKind::Ellipse).size(), std::size_t{1});
    editor()->cancel();

    // A spline through four points, ended with Enter.
    editor()->setTool(SketchTool::Spline);
    click(at(0.0, 60.0));
    click(at(20.0, 80.0));
    click(at(40.0, 60.0));
    click(at(60.0, 80.0));
    QVERIFY(curves(EntityKind::Spline).empty());
    render();
    keepFrame(overlayImage(), "sketch-spline-preview");
    QTest::keyClick(m_viewport, Qt::Key_Return);
    const std::vector<Entity> splines = curves(EntityKind::Spline);
    QCOMPARE(splines.size(), std::size_t{1});
    QCOMPARE(splines[0].points.size(), std::size_t{4});
    QVERIFY(apart(sketch().position(splines[0].start), {0.0, 60.0}) < 0.4);
    QVERIFY(apart(sketch().position(splines[0].end), {60.0, 80.0}) < 0.4);
    QCOMPARE(QString::fromStdString(m_document->undoLabel()), QStringLiteral("Spline"));
    // Another, ended with a double-click, starting on the first one's end: they are joined.
    click(at(60.0, 80.0));
    click(at(80.0, 100.0));
    QTest::mouseDClick(m_viewport, Qt::LeftButton, Qt::NoModifier, at(100.0, 80.0));
    render();
    QCOMPARE(curves(EntityKind::Spline).size(), std::size_t{2});
    QCOMPARE(curves(EntityKind::Spline)[1].points.size(), std::size_t{3});
    QCOMPARE(count(ConstraintKind::Coincident), 1);
    render();
    keepFrame(overlayImage(), "sketch-curves");

    // A point of a spline dragged with the Select tool reshapes it.
    editor()->setTool(SketchTool::Select);
    const Point2 before = sketch().pointAt(splines[0].id, 0.25);
    const QPoint grab = at(20.0, 80.0);
    QTest::mousePress(m_viewport, Qt::LeftButton, Qt::NoModifier, grab);
    QTest::mouseMove(m_viewport, grab + QPoint(5, -10));
    QTest::mouseMove(m_viewport, grab + QPoint(10, -30));
    QTest::mouseRelease(m_viewport, Qt::LeftButton, Qt::NoModifier, grab + QPoint(10, -30));
    QVERIFY(sketch().pointAt(splines[0].id, 0.25).y > before.y + 1.0);
}

void SketchViewTest::polygonAndSlotTools() {
    openSketch();
    editor()->setTool(SketchTool::Polygon);
    QCOMPARE(editor()->polygonSides(), 6);
    editor()->setPolygonSides(5);
    click(at(30.0, 30.0));
    click(at(50.0, 30.0));
    QCOMPARE(curves(EntityKind::Line).size(), std::size_t{5});
    QCOMPARE(curves(EntityKind::Circle).size(), std::size_t{1});
    QVERIFY(curves(EntityKind::Circle)[0].construction);
    QCOMPARE(sketch().profiles().size(), std::size_t{1});
    // A regular pentagon in a circle of radius 20.
    const double pentagon = 2.5 * 400.0 * std::sin(2.0 * M_PI / 5.0);
    QVERIFY2(qAbs(sketch().profiles()[0].area - pentagon) < 30.0,
             qPrintable(QString::number(sketch().profiles()[0].area)));
    editor()->setPolygonSides(2);
    QCOMPARE(editor()->polygonSides(), 3);

    editor()->setTool(SketchTool::Slot);
    click(at(70.0, 0.0));
    click(at(110.0, 0.0));
    click(at(90.0, 8.0));
    QCOMPARE(curves(EntityKind::Arc).size(), std::size_t{2});
    QCOMPARE(curves(EntityKind::Line).size(), std::size_t{7});
    QCOMPARE(sketch().profiles().size(), std::size_t{2});
    const double slot = 40.0 * 16.0 + M_PI * 64.0;
    const double found = std::min(sketch().profiles()[0].area, sketch().profiles()[1].area);
    QVERIFY2(qAbs(found - slot) < 30.0, qPrintable(QString::number(found)));
    QCOMPARE(QString::fromStdString(m_document->undoLabel()), QStringLiteral("Slot"));
}

void SketchViewTest::constructionModeDrawsConstructionGeometry() {
    openSketch();
    editor()->setConstructionMode(true);
    editor()->setTool(SketchTool::Line);
    click(at(10.0, 10.0));
    click(at(60.0, 40.0));
    editor()->cancel();
    editor()->setConstructionMode(false);
    click(at(10.0, 50.0));
    click(at(60.0, 60.0));
    const std::vector<Entity> lines = curves(EntityKind::Line);
    QCOMPARE(lines.size(), std::size_t{2});
    QVERIFY(lines[0].construction);
    QVERIFY(!lines[1].construction);
    // Construction geometry makes no outline.
    QVERIFY(sketch().profiles().empty());
}

void SketchViewTest::typingWhileDrawingGivesTheSize() {
    openSketch();
    m_viewport->setFocus();
    editor()->setTool(SketchTool::Line);
    click(at(10.0, 10.0));
    // Up and to the right, at 45 degrees.
    move(at(30.0, 30.0));
    QVERIFY(editor()->valueField() == nullptr);
    type(QStringLiteral("5"));
    QVERIFY(editor()->valueField() != nullptr);
    QCOMPARE(editor()->valueField()->text(), QStringLiteral("5"));
    type(QStringLiteral("0"));
    QTest::keyClick(editor()->valueField(), Qt::Key_Return);
    QVERIFY(editor()->valueField() == nullptr);

    std::vector<Entity> lines = curves(EntityKind::Line);
    QCOMPARE(lines.size(), std::size_t{1});
    // 50 mm long, in the direction the pointer gave, and dimensioned so.
    QVERIFY(qAbs(apart(startOf(lines[0].id), endOf(lines[0].id)) - 50.0) < 1e-6);
    QVERIFY(qAbs(endOf(lines[0].id).x - endOf(lines[0].id).y) < 0.5);
    QCOMPARE(count(ConstraintKind::Distance), 1);
    editor()->cancel();

    // A rectangle takes its width and its height.
    editor()->setTool(SketchTool::Rectangle);
    click(at(0.0, 0.0));
    move(at(20.0, -10.0));
    type(QStringLiteral("4"));
    type(QStringLiteral("0 25"));
    QTest::keyClick(editor()->valueField(), Qt::Key_Return);
    lines = curves(EntityKind::Line);
    QCOMPARE(lines.size(), std::size_t{5});
    QCOMPARE(count(ConstraintKind::Distance), 3);
    // To the right of and below the first corner, where the pointer was.
    QCOMPARE(sketch().profiles().size(), std::size_t{1});
    QVERIFY(qAbs(sketch().profiles()[0].area - 1000.0) < 1e-6);
    double lowest = 0.0;
    for (const Entity& line : lines) {
        lowest = std::min(lowest, startOf(line.id).y);
    }
    QVERIFY(qAbs(lowest + 25.0) < 1e-6);
    // Fully constrained: its corner is on the origin and both sizes are given. Only the first
    // line is still free to swing about.
    QVERIFY(editor()->state().degreesOfFreedom <= 3);

    // A circle takes its diameter. Nonsense is refused and left to be corrected.
    editor()->setTool(SketchTool::Circle);
    click(at(80.0, 40.0));
    move(at(85.0, 40.0));
    type(QStringLiteral("1"));
    type(QStringLiteral("x"));
    QTest::keyClick(editor()->valueField(), Qt::Key_Return);
    QVERIFY(editor()->valueField() != nullptr);
    QCOMPARE(editor()->valueField()->text(), QStringLiteral("1x"));
    QVERIFY(editor()->valueField()->property("invalid").toBool());
    QVERIFY(curves(EntityKind::Circle).empty());
    QTest::keyClick(editor()->valueField(), Qt::Key_Backspace);
    type(QStringLiteral("2"));
    QTest::keyClick(editor()->valueField(), Qt::Key_Return);
    QCOMPARE(curves(EntityKind::Circle).size(), std::size_t{1});
    QVERIFY(qAbs(curves(EntityKind::Circle)[0].radius - 6.0) < 1e-9);
    QCOMPARE(count(ConstraintKind::Diameter), 1);
}

// ---------------------------------------------------------------------------------------------
// Constraints and dimensions

void SketchViewTest::constraintsApplyToTheSelection() {
    openSketch();
    editor()->setInference(false);
    editor()->setTool(SketchTool::Line);
    click(at(10.0, 10.0));
    click(at(60.0, 14.0));
    editor()->cancel();
    click(at(12.0, 40.0));
    click(at(64.0, 50.0));
    editor()->cancel();
    editor()->cancel();
    const std::vector<Entity> lines = curves(EntityKind::Line);

    // Click one line, Shift + click the other, and make them parallel.
    click(at(35.0, 12.0));
    QCOMPARE(editor()->selectedEntities(), (std::vector<EntityId>{lines[0].id}));
    click(at(38.0, 45.0), Qt::ShiftModifier);
    QCOMPARE(editor()->selectedEntities().size(), std::size_t{2});
    QVERIFY(editor()->constrain(ConstraintKind::Parallel));
    QCOMPARE(count(ConstraintKind::Parallel), 1);
    QCOMPARE(QString::fromStdString(m_document->undoLabel()), QStringLiteral("Parallel"));
    const Point2 d1{endOf(lines[0].id).x - startOf(lines[0].id).x,
                    endOf(lines[0].id).y - startOf(lines[0].id).y};
    const Point2 d2{endOf(lines[1].id).x - startOf(lines[1].id).x,
                    endOf(lines[1].id).y - startOf(lines[1].id).y};
    QVERIFY(qAbs(d1.x * d2.y - d1.y * d2.x) < 1e-6);
    // The selection is used up.
    QVERIFY(editor()->selectedEntities().empty());

    // Horizontal takes several lines at once, each on its own. One of the two is enough to
    // level both, so the second would add nothing and is left out, with a word about it.
    QSignalSpy messages(editor(), &SketchEditor::message);
    editor()->setSelection({lines[0].id, lines[1].id});
    QVERIFY(editor()->constrain(ConstraintKind::Horizontal));
    QCOMPARE(count(ConstraintKind::Horizontal), 1);
    QVERIFY(qAbs(endOf(lines[1].id).y - startOf(lines[1].id).y) < 1e-6);
    QCOMPARE(messages.count(), 1);

    // Coincident on two ends joins them.
    editor()->setSelection({lines[0].end, lines[1].start});
    QVERIFY(editor()->constrain(ConstraintKind::Coincident));
    QVERIFY(apart(endOf(lines[0].id), startOf(lines[1].id)) < 1e-6);
}

void SketchViewTest::aConstraintWithoutAFittingSelectionBecomesTheTool() {
    openSketch();
    editor()->setInference(false);
    editor()->setTool(SketchTool::Line);
    click(at(10.0, 10.0));
    click(at(60.0, 14.0));
    editor()->cancel();
    click(at(12.0, 40.0));
    click(at(20.0, 70.0));
    editor()->cancel();
    editor()->cancel();
    const std::vector<Entity> lines = curves(EntityKind::Line);

    // Nothing is selected: the constraint waits for clicks, and says what it takes.
    QSignalSpy messages(editor(), &SketchEditor::message);
    QVERIFY(!editor()->constrain(ConstraintKind::Perpendicular));
    QCOMPARE(editor()->tool(), SketchTool::Constraint);
    QCOMPARE(editor()->constraintTool(), ConstraintKind::Perpendicular);
    QCOMPARE(messages.count(), 1);
    QVERIFY(messages.first().first().toString().contains(QStringLiteral("two lines")));
    QVERIFY(editor()->hint().contains(QStringLiteral("Perpendicular")));

    click(at(35.0, 12.0));
    QCOMPARE(count(ConstraintKind::Perpendicular), 0);
    click(at(16.0, 55.0));
    QCOMPARE(count(ConstraintKind::Perpendicular), 1);
    const Point2 d1{endOf(lines[0].id).x - startOf(lines[0].id).x,
                    endOf(lines[0].id).y - startOf(lines[0].id).y};
    const Point2 d2{endOf(lines[1].id).x - startOf(lines[1].id).x,
                    endOf(lines[1].id).y - startOf(lines[1].id).y};
    QVERIFY(qAbs(d1.x * d2.x + d1.y * d2.y) < 1e-6);
    // The tool stays for the next pair. A one-click constraint applies at once.
    QCOMPARE(editor()->tool(), SketchTool::Constraint);
    editor()->setConstraintTool(ConstraintKind::Horizontal);
    click(at(35.0, 12.0));
    QCOMPARE(count(ConstraintKind::Horizontal), 1);
    QVERIFY(qAbs(endOf(lines[0].id).y - startOf(lines[0].id).y) < 1e-6);
}

void SketchViewTest::redundantAndConflictingConstraintsAreRefused() {
    openSketch();
    editor()->setTool(SketchTool::Rectangle);
    click(at(0.0, 0.0));
    click(at(80.0, 50.0));
    editor()->cancel();
    editor()->cancel();
    const std::vector<Entity> lines = curves(EntityKind::Line);
    const std::size_t constraints = sketch().constraints().size();
    const int steps = [this] {
        int n = 0;
        // Count the undo steps by undoing a copy of nothing: the label suffices here.
        return n;
    }();
    Q_UNUSED(steps)
    const QString lastStep = QString::fromStdString(m_document->undoLabel());

    QSignalSpy messages(editor(), &SketchEditor::message);
    // Opposite sides of a rectangle are parallel already.
    editor()->setSelection({lines[0].id, lines[2].id});
    QVERIFY(!editor()->constrain(ConstraintKind::Parallel));
    QCOMPARE(messages.count(), 1);
    QVERIFY(messages.last().first().toString().contains(QStringLiteral("over-constrain")));
    // And they cannot be square to each other.
    QVERIFY(!editor()->constrain(ConstraintKind::Perpendicular));
    QCOMPARE(messages.count(), 2);
    QVERIFY(messages.last().first().toString().contains(QStringLiteral("contradicts")));

    // Nothing was added and nothing can be undone that was not there before.
    QCOMPARE(sketch().constraints().size(), constraints);
    QCOMPARE(QString::fromStdString(m_document->undoLabel()), lastStep);
    QCOMPARE(editor()->state().status, lcad::sketch::SolveStatus::Solved);
}

// The acceptance test of Phase 2, in the 3D view: draw a rectangle, constrain it fully with two
// dimensions typed in the view, change a dimension, and measure that the geometry followed.
void SketchViewTest::aFullyConstrainedRectangleFollowsItsDimensions() {
    openSketch();
    editor()->setTool(SketchTool::Rectangle);
    click(at(0.0, 0.0));
    click(at(80.0, 50.0));
    QCOMPARE(editor()->state().degreesOfFreedom, 2);
    QVERIFY(!editor()->state().isFullyConstrained());
    const std::vector<Entity> lines = curves(EntityKind::Line);
    const EntityId bottom = lines[0].id;
    const EntityId right = lines[1].id;

    // The width: click the bottom line, then empty space, and type.
    editor()->setTool(SketchTool::Dimension);
    click(at(40.0, 0.0));
    QVERIFY(editor()->valueField() == nullptr);
    click(at(40.0, -15.0));
    QVERIFY(editor()->valueField() != nullptr);
    // The field shows what the line measures now, selected, ready to be typed over.
    QVERIFY(qAbs(editor()->valueField()->text().toDouble() - 80.0) < 0.4);
    QCOMPARE(editor()->valueField()->selectedText(), editor()->valueField()->text());
    type(QStringLiteral("60"));
    QTest::keyClick(editor()->valueField(), Qt::Key_Return);
    QVERIFY(editor()->valueField() == nullptr);
    QCOMPARE(editor()->state().degreesOfFreedom, 1);

    // The height, with its unit.
    click(at(60.0, 25.0));
    click(at(90.0, 25.0));
    QVERIFY(editor()->valueField() != nullptr);
    type(QStringLiteral("35 mm"));
    QTest::keyClick(editor()->valueField(), Qt::Key_Return);
    editor()->cancel();
    editor()->cancel();

    QVERIFY(editor()->state().isFullyConstrained());
    QVERIFY(editor()->state().underconstrained.empty());
    QVERIFY(apart(startOf(bottom), {0.0, 0.0}) < 1e-9);
    QVERIFY(apart(endOf(bottom), {60.0, 0.0}) < 1e-6);
    QVERIFY(apart(endOf(right), {60.0, 35.0}) < 1e-6);
    QCOMPARE(sketch().profiles().size(), std::size_t{1});
    QVERIFY(qAbs(sketch().profiles()[0].area - 2100.0) < 1e-6);
    QCOMPARE(count(ConstraintKind::Distance), 2);

    // Change the width: double-click its value and type another.
    ConstraintId width = 0;
    for (const Constraint& constraint : sketch().constraints()) {
        if (constraint.kind == ConstraintKind::Distance && constraint.value == 60.0) {
            width = constraint.id;
        }
    }
    QVERIFY(width != 0);
    const QRectF label = editor()->dimensionRect(width);
    QVERIFY(!label.isEmpty());
    // The value is drawn near what it measures: below the bottom line.
    QVERIFY(label.center().y() > at(30.0, 0.0).y());
    QVERIFY(qAbs(label.center().x() - at(30.0, 0.0).x()) < 4.0);
    QTest::mouseMove(m_viewport, label.center().toPoint());
    QTest::mouseDClick(m_viewport, Qt::LeftButton, Qt::NoModifier, label.center().toPoint());
    render();
    QVERIFY(editor()->valueField() != nullptr);
    QCOMPARE(editor()->valueField()->text(), QStringLiteral("60"));
    type(QStringLiteral("92.5"));
    QTest::keyClick(editor()->valueField(), Qt::Key_Return);

    QVERIFY(editor()->state().isFullyConstrained());
    QVERIFY(apart(endOf(bottom), {92.5, 0.0}) < 1e-6);
    QVERIFY(apart(endOf(right), {92.5, 35.0}) < 1e-6);
    QVERIFY(qAbs(sketch().profiles()[0].area - 92.5 * 35.0) < 1e-6);
    QCOMPARE(QString::fromStdString(m_document->undoLabel()), QStringLiteral("Change Dimension"));
    QVERIFY(qAbs(editor()->dimensionValue(width) - 92.5) < 1e-12);

    // Undo puts the old width back, and the drawing follows.
    m_document->undo();
    QVERIFY(apart(endOf(bottom), {60.0, 0.0}) < 1e-6);
    m_document->redo();
    QVERIFY(apart(endOf(bottom), {92.5, 0.0}) < 1e-6);
    keepFrame(m_viewport->grab().toImage(), "sketch-acceptance");
}

void SketchViewTest::anImpossibleOrMeaninglessValueIsKeptForCorrection() {
    openSketch();
    // A triangle with all three sides dimensioned.
    Sketch geometry = sketch();
    const EntityId a = geometry.addLine({0.0, 0.0}, {30.0, 0.0});
    const EntityId b = geometry.addLine({30.0, 0.0}, {0.0, 40.0});
    const EntityId c = geometry.addLine({0.0, 40.0}, {0.0, 0.0});
    const auto join = [&geometry](EntityId first, EntityId second) {
        geometry.addConstraint(ConstraintKind::Coincident, geometry.find(first)->end,
                               geometry.find(second)->start);
    };
    join(a, b);
    join(b, c);
    join(c, a);
    geometry.addConstraint(ConstraintKind::Distance, geometry.find(a)->start, geometry.find(a)->end,
                           0, 30.0);
    const ConstraintId longest = geometry.addConstraint(
        ConstraintKind::Distance, geometry.find(b)->start, geometry.find(b)->end, 0, 50.0);
    geometry.addConstraint(ConstraintKind::Distance, geometry.find(c)->start, geometry.find(c)->end,
                           0, 40.0);
    m_document->setSketchGeometry(m_sketch, geometry, "Triangle");
    render();

    QSignalSpy messages(editor(), &SketchEditor::message);
    editor()->editDimension(longest);
    QVERIFY(editor()->valueField() != nullptr);
    // Not a number.
    type(QStringLiteral("abc"));
    QTest::keyClick(editor()->valueField(), Qt::Key_Return);
    QVERIFY(editor()->valueField() != nullptr);
    QCOMPARE(editor()->valueField()->text(), QStringLiteral("abc"));
    QVERIFY(editor()->valueField()->property("invalid").toBool());
    QVERIFY(!editor()->valueField()->toolTip().isEmpty());
    QCOMPARE(messages.count(), 1);
    // Typing again takes the mark away.
    QTest::keyClick(editor()->valueField(), Qt::Key_A, Qt::ControlModifier);
    type(QStringLiteral("80"));
    QVERIFY(!editor()->valueField()->property("invalid").toBool());
    // A number the triangle cannot have: sides of 30 and 40 do not reach across 80.
    QTest::keyClick(editor()->valueField(), Qt::Key_Return);
    QVERIFY(editor()->valueField() != nullptr);
    QCOMPARE(editor()->valueField()->text(), QStringLiteral("80"));
    QVERIFY(editor()->valueField()->property("invalid").toBool());
    QCOMPARE(messages.count(), 2);
    QVERIFY(messages.last().first().toString().contains(QStringLiteral("contradicts")));
    // The sketch is as it was.
    QCOMPARE(sketch().findConstraint(longest)->value, 50.0);
    QVERIFY(apart(endOf(b), {0.0, 40.0}) < 1e-6);
    QCOMPARE(QString::fromStdString(m_document->undoLabel()), QStringLiteral("Triangle"));

    // Esc gives the value up.
    editor()->cancel();
    QVERIFY(editor()->valueField() == nullptr);
    QCOMPARE(sketch().findConstraint(longest)->value, 50.0);
    // And a value it can have is taken.
    editor()->editDimension(longest);
    type(QStringLiteral("45,5"));
    QTest::keyClick(editor()->valueField(), Qt::Key_Return);
    QVERIFY(editor()->valueField() == nullptr);
    QCOMPARE(sketch().findConstraint(longest)->value, 45.5);
}

void SketchViewTest::markersAndValuesCanBePickedAndDeleted() {
    openSketch();
    editor()->setTool(SketchTool::Rectangle);
    click(at(10.0, 10.0));
    click(at(80.0, 50.0));
    editor()->cancel();
    editor()->cancel();
    ConstraintId level = 0;
    for (const Constraint& constraint : sketch().constraints()) {
        if (constraint.kind == ConstraintKind::Horizontal && level == 0) {
            level = constraint.id;
        }
    }
    const QRectF marker = editor()->markerRect(level);
    QVERIFY(!marker.isEmpty());
    // Near the middle of the line it belongs to.
    QVERIFY(QLineF(marker.center(), at(45.0, 10.0)).length() < 40.0);

    click(marker.center().toPoint());
    QCOMPARE(editor()->selectedConstraints(), (std::vector<ConstraintId>{level}));
    QVERIFY(editor()->selectedEntities().empty());
    QVERIFY(editor()->deleteSelection());
    QVERIFY(sketch().findConstraint(level) == nullptr);
    QCOMPARE(count(ConstraintKind::Horizontal), 1);
    QCOMPARE(editor()->state().degreesOfFreedom, 5);
    QCOMPARE(QString::fromStdString(m_document->undoLabel()), QStringLiteral("Delete"));
    QVERIFY(editor()->selectedConstraints().empty());
    // With nothing selected there is nothing to delete.
    QVERIFY(!editor()->deleteSelection());
}

// ---------------------------------------------------------------------------------------------
// Selecting and moving

void SketchViewTest::clicksAndABoxSelect() {
    openSketch();
    editor()->setTool(SketchTool::Rectangle);
    click(at(10.0, 10.0));
    click(at(50.0, 40.0));
    editor()->setTool(SketchTool::Circle);
    click(at(80.0, 25.0));
    click(at(90.0, 25.0));
    editor()->setTool(SketchTool::Select);
    const std::vector<Entity> lines = curves(EntityKind::Line);
    const Entity circle = curves(EntityKind::Circle).front();

    QSignalSpy changed(editor(), &SketchEditor::changed);
    click(at(30.0, 10.0));
    QCOMPARE(editor()->selectedEntities(), (std::vector<EntityId>{lines[0].id}));
    QVERIFY(changed.count() >= 1);
    // An end is picked in preference to the line it ends.
    click(at(50.0, 10.0));
    QCOMPARE(editor()->selectedEntities().size(), std::size_t{1});
    QCOMPARE(sketch().find(editor()->selectedEntities()[0])->kind, EntityKind::Point);
    // Ctrl + click adds, and again takes away.
    click(at(90.0, 25.0), Qt::ControlModifier);
    QCOMPARE(editor()->selectedEntities().size(), std::size_t{2});
    click(at(90.0, 25.0), Qt::ControlModifier);
    QCOMPARE(editor()->selectedEntities().size(), std::size_t{1});
    // A click on nothing clears.
    click(at(100.0, 70.0));
    QVERIFY(editor()->selectedEntities().empty());

    // A box takes what lies wholly inside it: the rectangle, not the circle.
    drag(at(5.0, 5.0), at(60.0, 45.0));
    QCOMPARE(editor()->selectedEntities().size(), std::size_t{4});
    const std::vector<EntityId> boxed = editor()->selectedEntities();
    QVERIFY(std::find(boxed.begin(), boxed.end(), circle.id) == boxed.end());

    QVERIFY(m_document->undoLabel() == "Circle");
}

void SketchViewTest::draggingMovesFreeGeometryAsOneUndoStep() {
    openSketch();
    editor()->setTool(SketchTool::Rectangle);
    click(at(0.0, 0.0));
    click(at(60.0, 40.0));
    editor()->setTool(SketchTool::Select);
    const std::vector<Entity> lines = curves(EntityKind::Line);
    const EntityId right = lines[1].id;

    // Pull the top right corner out: the rectangle stays a rectangle and grows.
    drag(at(60.0, 40.0), at(90.0, 55.0));
    QVERIFY2(apart(endOf(right), {90.0, 55.0}) < 1.0,
             qPrintable(QStringLiteral("%1 %2").arg(endOf(right).x).arg(endOf(right).y)));
    QVERIFY(qAbs(startOf(right).x - endOf(right).x) < 1e-6);
    QVERIFY(apart(startOf(lines[0].id), {0.0, 0.0}) < 1e-9);
    QCOMPARE(QString::fromStdString(m_document->undoLabel()), QStringLiteral("Drag"));
    // One step, however many positions the pointer went through.
    m_document->undo();
    QVERIFY(apart(endOf(right), {60.0, 40.0}) < 0.4);
    QCOMPARE(QString::fromStdString(m_document->undoLabel()), QStringLiteral("Rectangle"));
    m_document->redo();

    // A circle is resized by its rim and moved by its center.
    editor()->setTool(SketchTool::Circle);
    click(at(30.0, -40.0));
    click(at(40.0, -40.0));
    editor()->setTool(SketchTool::Select);
    const Entity circle = curves(EntityKind::Circle).front();
    drag(at(40.0, -40.0), at(46.0, -40.0));
    QVERIFY2(qAbs(sketch().radiusOf(circle.id) - 16.0) < 0.5,
             qPrintable(QString::number(sketch().radiusOf(circle.id))));
    QVERIFY(apart(sketch().position(circle.center), {30.0, -40.0}) < 0.4);

    // Geometry that cannot move does not, and leaves no empty undo step behind.
    Sketch pinned = sketch();
    pinned.addConstraint(ConstraintKind::Fix, circle.center);
    pinned.addConstraint(ConstraintKind::Radius, circle.id, 0, 0, 16.0);
    pinned.solve();
    m_document->setSketchGeometry(m_sketch, pinned, "Pin");
    drag(at(46.0, -40.0), at(60.0, -40.0));
    QVERIFY(qAbs(sketch().radiusOf(circle.id) - 16.0) < 1e-9);
    QCOMPARE(QString::fromStdString(m_document->undoLabel()), QStringLiteral("Pin"));
}

void SketchViewTest::deleteAndConstructionActOnTheSelection() {
    openSketch();
    editor()->setTool(SketchTool::Rectangle);
    click(at(10.0, 10.0));
    click(at(60.0, 40.0));
    editor()->setTool(SketchTool::Select);
    const std::vector<Entity> lines = curves(EntityKind::Line);

    // The end of a line cannot go on its own.
    QSignalSpy messages(editor(), &SketchEditor::message);
    editor()->setSelection({lines[0].end});
    QVERIFY(!editor()->deleteSelection());
    QCOMPARE(messages.count(), 1);
    QCOMPARE(curves(EntityKind::Line).size(), std::size_t{4});

    editor()->setSelection({lines[2].id});
    QVERIFY(editor()->toggleConstruction());
    QVERIFY(sketch().find(lines[2].id)->construction);
    QVERIFY(sketch().profiles().empty());
    editor()->setSelection({lines[2].id});
    QVERIFY(editor()->toggleConstruction());
    QVERIFY(!sketch().find(lines[2].id)->construction);
    QCOMPARE(sketch().profiles().size(), std::size_t{1});

    // With nothing selected, it switches what the drawing tools make.
    editor()->clearSelection();
    QVERIFY(!editor()->toggleConstruction());
    QVERIFY(editor()->constructionMode());
    editor()->setConstructionMode(false);

    editor()->setSelection({lines[2].id, lines[3].id});
    QVERIFY(editor()->deleteSelection());
    QCOMPARE(curves(EntityKind::Line).size(), std::size_t{2});
    // The constraints that were about them went too.
    QCOMPARE(count(ConstraintKind::Coincident), 1);
    QCOMPARE(QString::fromStdString(m_document->undoLabel()), QStringLiteral("Delete"));
    m_document->undo();
    QCOMPARE(curves(EntityKind::Line).size(), std::size_t{4});
}

// ---------------------------------------------------------------------------------------------
// Modifying

void SketchViewTest::trimAndExtendActOnTheCurveClicked() {
    openSketch();
    editor()->setInference(false);
    editor()->setTool(SketchTool::Line);
    const auto line = [this](Point2 from, Point2 to) {
        click(at(from.x, from.y));
        click(at(to.x, to.y));
        editor()->cancel();
    };
    line({0.0, 20.0}, {100.0, 20.0});
    line({30.0, 0.0}, {30.0, 50.0});
    line({70.0, 0.0}, {70.0, 50.0});
    line({10.0, 60.0}, {40.0, 60.0});
    const std::vector<Entity> lines = curves(EntityKind::Line);
    QCOMPARE(lines.size(), std::size_t{4});

    // Trim the middle of the long line: two pieces are left.
    editor()->setTool(SketchTool::Trim);
    click(at(50.0, 20.0));
    QCOMPARE(curves(EntityKind::Line).size(), std::size_t{5});
    QVERIFY(apart(endOf(lines[0].id), {30.0, 20.0}) < 0.4);
    QCOMPARE(QString::fromStdString(m_document->undoLabel()), QStringLiteral("Trim"));
    // Nothing under the pointer, nothing trimmed, and a word about it.
    QSignalSpy messages(editor(), &SketchEditor::message);
    click(at(50.0, 35.0));
    QCOMPARE(curves(EntityKind::Line).size(), std::size_t{5});
    QCOMPARE(messages.count(), 1);

    // Extend the short line at its right end: it reaches the second upright line.
    editor()->setTool(SketchTool::Extend);
    click(at(38.0, 60.0));
    QCOMPARE(messages.count(), 2); // That end meets nothing: both uprights stop at y = 50.
    Sketch taller = sketch();
    m_document->undo(); // not the extend, which did nothing: the trim
    QCOMPARE(curves(EntityKind::Line).size(), std::size_t{4});
    Q_UNUSED(taller)

    // Draw a wall for it to meet.
    editor()->setTool(SketchTool::Line);
    line({90.0, 40.0}, {90.0, 70.0});
    editor()->setTool(SketchTool::Extend);
    click(at(38.0, 60.0));
    QVERIFY2(apart(endOf(lines[3].id), {90.0, 60.0}) < 0.4,
             qPrintable(QString::number(endOf(lines[3].id).x)));
    QCOMPARE(QString::fromStdString(m_document->undoLabel()), QStringLiteral("Extend"));
    QCOMPARE(count(ConstraintKind::PointOnLine), 1);
}

void SketchViewTest::offsetAndMirror() {
    openSketch();
    editor()->setTool(SketchTool::Rectangle);
    click(at(20.0, 10.0));
    click(at(60.0, 40.0));

    // Offset: click a curve, which takes the outline it is part of, then where the copy goes.
    editor()->setTool(SketchTool::Offset);
    click(at(40.0, 10.0));
    QCOMPARE(editor()->selectedEntities().size(), std::size_t{4});
    click(at(40.0, 4.0));
    QCOMPARE(curves(EntityKind::Line).size(), std::size_t{8});
    QCOMPARE(sketch().profiles().size(), std::size_t{2});
    const double outer = std::max(sketch().profiles()[0].area, sketch().profiles()[1].area);
    // 6 mm out all round: 52 by 42.
    QVERIFY2(qAbs(outer - 52.0 * 42.0) < 60.0, qPrintable(QString::number(outer)));
    QCOMPARE(QString::fromStdString(m_document->undoLabel()), QStringLiteral("Offset"));
    m_document->undo();
    QCOMPARE(curves(EntityKind::Line).size(), std::size_t{4});

    // Mirror: without a selection it says what to do first.
    QSignalSpy messages(editor(), &SketchEditor::message);
    editor()->setTool(SketchTool::Select);
    editor()->clearSelection();
    editor()->setTool(SketchTool::Mirror);
    QCOMPARE(editor()->tool(), SketchTool::Select);
    QCOMPARE(messages.count(), 1);

    // A line to mirror in, at x = 80.
    editor()->setTool(SketchTool::Line);
    click(at(80.0, -10.0));
    click(at(80.0, 60.0));
    editor()->cancel();
    const std::vector<Entity> lines = curves(EntityKind::Line);
    editor()->setTool(SketchTool::Select);
    editor()->setSelection({lines[0].id, lines[1].id, lines[2].id, lines[3].id});
    editor()->setTool(SketchTool::Mirror);
    QCOMPARE(editor()->tool(), SketchTool::Mirror);
    click(at(80.0, 30.0));
    QCOMPARE(curves(EntityKind::Line).size(), std::size_t{9});
    QCOMPARE(count(ConstraintKind::Symmetric), 8);
    QCOMPARE(sketch().profiles().size(), std::size_t{2});
    QCOMPARE(editor()->tool(), SketchTool::Select);
    QCOMPARE(QString::fromStdString(m_document->undoLabel()), QStringLiteral("Mirror"));
    // The image of the corner at (20, 10) is at (140, 10).
    bool found = false;
    for (const Entity& line : curves(EntityKind::Line)) {
        found = found || apart(startOf(line.id), {140.0, 10.0}) < 0.5 ||
                apart(endOf(line.id), {140.0, 10.0}) < 0.5;
    }
    QVERIFY(found);
}

void SketchViewTest::projectBringsEdgesOfABodyIntoTheSketch() {
    // A box from the origin to (100, 60, 40), and a sketch on its top face.
    m_document->addBody("Box", lcad::kernel::makeBox(100.0, 60.0, 40.0));
    lcad::kernel::Plane top = lcad::kernel::planeXY();
    top.origin = {0.0, 0.0, 40.0};
    m_sketch = m_document->addSketch("Sketch 1", top);
    QVERIFY(m_viewport->editSketch(m_sketch));
    render();
    // With any other tool the box is not there to be pointed at.
    QVERIFY(m_viewport->itemsAt(at(50.0, 0.0)).empty());

    editor()->setTool(SketchTool::Project);
    QVERIFY(!editor()->hint().isEmpty());
    render();
    QVERIFY(!m_viewport->itemsAt(at(50.0, 0.0)).empty());

    // The front edge of the top face.
    click(at(50.0, 0.0));
    std::vector<Entity> lines = curves(EntityKind::Line);
    QCOMPARE(lines.size(), std::size_t{1});
    QVERIFY(lines[0].fixed);
    const Point2 a = startOf(lines[0].id);
    const Point2 b = endOf(lines[0].id);
    QVERIFY(qAbs(a.y) < 1e-9 && qAbs(b.y) < 1e-9);
    QVERIFY(qAbs(qAbs(b.x - a.x) - 100.0) < 1e-9);
    QCOMPARE(QString::fromStdString(m_document->undoLabel()), QStringLiteral("Project"));
    // Reference geometry cannot move, so it leaves the sketch fully constrained.
    QVERIFY(editor()->state().isFullyConstrained());

    // A corner comes in as a point. (The far left one: the right one is under the view cube.)
    click(at(0.0, 60.0));
    const std::vector<Entity> points = curves(EntityKind::Point);
    QCOMPARE(points.size(), std::size_t{1});
    QVERIFY(points[0].fixed);
    QVERIFY(apart({points[0].x, points[0].y}, {0.0, 60.0}) < 1e-9);

    // The middle of a face is neither an edge nor a corner: nothing is projected, and the
    // tool says what it takes.
    QSignalSpy messages(editor(), &SketchEditor::message);
    click(at(50.0, 30.0));
    QCOMPARE(curves(EntityKind::Line).size(), std::size_t{1});
    QCOMPARE(curves(EntityKind::Point).size(), std::size_t{1});
    QCOMPARE(messages.count(), 1);

    // It is drawn in the color of reference geometry.
    editor()->setTool(SketchTool::Select);
    move(at(50.0, 30.0));
    const QImage image = overlayImage();
    const QPoint onTheLine = (editor()->toWidget({30.0, 0.0}) * image.devicePixelRatio()).toPoint();
    bool seen = false;
    for (int dy = -3; dy <= 3 && !seen; ++dy) {
        const QPoint p = onTheLine + QPoint(0, dy);
        seen = qAlpha(image.pixel(p)) > 200 &&
               isNear(image.pixel(p), m_viewport->colors().sketchReference, 30);
    }
    QVERIFY(seen);
    // With the tool put away, the box cannot be pointed at any more.
    QVERIFY(m_viewport->itemsAt(at(50.0, 0.0)).empty());

    // New geometry is tied to it like to any other: a circle centered on the projected corner
    // has only its radius left to decide. And the reference does not get dragged along.
    editor()->setTool(SketchTool::Circle);
    click(at(0.0, 60.0));
    click(at(0.0, 45.0));
    QCOMPARE(editor()->state().degreesOfFreedom, 1);
    editor()->setTool(SketchTool::Select);
    drag(at(50.0, 0.0), at(50.0, 20.0));
    QVERIFY(qAbs(startOf(lines[0].id).y) < 1e-9);
    // It can be deleted like anything else.
    editor()->setSelection({lines[0].id});
    QVERIFY(editor()->deleteSelection());
    QVERIFY(curves(EntityKind::Line).empty());
}

// With the Dimension tool, a line takes one click: its dimension then follows the pointer, and
// typing a number places it there with that value. Enter places it as it measures.
void SketchViewTest::aLineIsDimensionedByClickingIt() {
    openSketch();
    m_viewport->setFocus();
    editor()->setInference(false);
    editor()->setTool(SketchTool::Line);
    for (const double y : {0.0, 40.0}) {
        click(at(10.0, y));
        click(at(70.0, y));
        editor()->cancel();
    }
    const std::vector<Entity> lines = curves(EntityKind::Line);
    QCOMPARE(lines.size(), std::size_t{2});
    editor()->setTool(SketchTool::Dimension);
    QVERIFY2(editor()->hint().contains(QStringLiteral("click a line")),
             qPrintable(editor()->hint()));

    // Anything drawn near a place of the sketch.
    const auto drawnNear = [this](const QImage& image, Point2 where) {
        const QPoint pixel = (editor()->toWidget(where) * image.devicePixelRatio()).toPoint();
        const int reach = qRound(4 * image.devicePixelRatio());
        for (int dy = -reach; dy <= reach; ++dy) {
            for (int dx = -reach; dx <= reach; ++dx) {
                const QPoint p = pixel + QPoint(dx, dy);
                if (image.rect().contains(p) && qAlpha(image.pixel(p)) > 0) {
                    return true;
                }
            }
        }
        return false;
    };
    // The value of a length goes 38 pixels below the line, which is 12.7 mm at 3 pixels to the
    // millimeter.
    const Point2 label{40.0, -38.0 / 3.0};
    move(at(40.0, -12.0));
    QVERIFY(!drawnNear(overlayImage(), label));

    click(at(40.0, 0.0));
    QVERIFY(editor()->valueField() == nullptr);
    QVERIFY2(editor()->hint().contains(QStringLiteral("type the length")),
             qPrintable(editor()->hint()));
    move(at(40.0, -12.0));
    QImage image = overlayImage();
    keepFrame(image, "sketch-dimension-preview");
    QVERIFY(drawnNear(image, label));
    QCOMPARE(count(ConstraintKind::Distance), 0);

    // Typing places it and starts its value.
    type(QStringLiteral("5"));
    QVERIFY(editor()->valueField() != nullptr);
    QCOMPARE(editor()->valueField()->text(), QStringLiteral("5"));
    type(QStringLiteral("0"));
    QTest::keyClick(editor()->valueField(), Qt::Key_Return);
    QVERIFY(editor()->valueField() == nullptr);
    QCOMPARE(count(ConstraintKind::Distance), 1);
    QVERIFY(qAbs(apart(startOf(lines[0].id), endOf(lines[0].id)) - 50.0) < 1e-6);
    QCOMPARE(editor()->tool(), SketchTool::Dimension);

    // Enter places it as it measures, ready to be typed over.
    click(at(40.0, 40.0));
    QTest::keyClick(m_viewport, Qt::Key_Return);
    QVERIFY(editor()->valueField() != nullptr);
    QVERIFY(qAbs(editor()->valueField()->text().toDouble() - 60.0) < 0.4);
    type(QStringLiteral("45"));
    QTest::keyClick(editor()->valueField(), Qt::Key_Return);
    QCOMPARE(count(ConstraintKind::Distance), 2);
    QVERIFY(qAbs(apart(startOf(lines[1].id), endOf(lines[1].id)) - 45.0) < 1e-6);

    // With nothing picked, a number is not a value for anything.
    type(QStringLiteral("7"));
    QVERIFY(editor()->valueField() == nullptr);
    QCOMPARE(count(ConstraintKind::Distance), 2);
}

void SketchViewTest::aDimensionIsHorizontalOrVerticalByWhereItIsPut() {
    openSketch();
    editor()->setInference(false);
    editor()->setTool(SketchTool::Line);
    // Three slanted lines, each 40 across and 30 up.
    for (const double y : {0.0, -40.0, 40.0}) {
        click(at(10.0, y));
        click(at(50.0, y + 30.0));
        editor()->cancel();
    }
    const std::vector<Entity> lines = curves(EntityKind::Line);
    QCOMPARE(lines.size(), std::size_t{3});
    editor()->setTool(SketchTool::Dimension);
    const auto dimensioned = [this](ConstraintKind kind) {
        for (const Constraint& constraint : sketch().constraints()) {
            if (constraint.kind == kind) {
                return constraint.value;
            }
        }
        return -1.0;
    };

    // Put below the line, between its ends: how far it runs sideways.
    click(at(30.0, 15.0));
    click(at(30.0, -8.0));
    QVERIFY(editor()->valueField() != nullptr);
    QVERIFY(qAbs(editor()->valueField()->text().toDouble() - 40.0) < 0.5);
    type(QStringLiteral("44"));
    QTest::keyClick(editor()->valueField(), Qt::Key_Return);
    QCOMPARE(dimensioned(ConstraintKind::DistanceX), 44.0);
    QVERIFY(qAbs(qAbs(endOf(lines[0].id).x - startOf(lines[0].id).x) - 44.0) < 1e-6);

    // Put beside the second line, level with it: how far it runs up.
    click(at(30.0, -25.0));
    click(at(70.0, -25.0));
    QVERIFY(editor()->valueField() != nullptr);
    type(QStringLiteral("25"));
    QTest::keyClick(editor()->valueField(), Qt::Key_Return);
    QCOMPARE(dimensioned(ConstraintKind::DistanceY), 25.0);
    QVERIFY(qAbs(qAbs(endOf(lines[1].id).y - startOf(lines[1].id).y) - 25.0) < 1e-6);

    // Put off to a corner of the third: its length, straight across.
    click(at(30.0, 55.0));
    click(at(70.0, 90.0));
    QVERIFY(editor()->valueField() != nullptr);
    QVERIFY(qAbs(editor()->valueField()->text().toDouble() - 50.0) < 0.5);
    QTest::keyClick(editor()->valueField(), Qt::Key_Return);
    QCOMPARE(count(ConstraintKind::Distance), 1);
    QCOMPARE(count(ConstraintKind::DistanceX), 1);
    QCOMPARE(count(ConstraintKind::DistanceY), 1);

    // Each is drawn with its value near what it measures, and can be opened again.
    for (const Constraint& constraint : sketch().constraints()) {
        const QRectF label = editor()->dimensionRect(constraint.id);
        QVERIFY(!label.isEmpty());
        QVERIFY(m_viewport->rect().contains(label.toRect()));
    }
}

// ---------------------------------------------------------------------------------------------
// Drawing on screen

void SketchViewTest::geometryIsColoredByItsState() {
    openSketch();
    editor()->setTool(SketchTool::Rectangle);
    click(at(0.0, 0.0));
    click(at(80.0, 50.0));
    editor()->setTool(SketchTool::Select);
    move(at(110.0, 70.0));
    const lcad::viewport::ViewportColors colors = m_viewport->colors();
    const std::vector<Entity> lines = curves(EntityKind::Line);

    // Looks for a color near a place of the sketch in what the editor drew.
    const auto hasColor = [this](const QImage& image, Point2 where, const QColor& color) {
        const QPoint pixel = (editor()->toWidget(where) * image.devicePixelRatio()).toPoint();
        const int reach = qRound(3 * image.devicePixelRatio());
        for (int dy = -reach; dy <= reach; ++dy) {
            for (int dx = -reach; dx <= reach; ++dx) {
                const QPoint p = pixel + QPoint(dx, dy);
                if (image.rect().contains(p) && qAlpha(image.pixel(p)) > 200 &&
                    isNear(image.pixel(p), color, 30)) {
                    return true;
                }
            }
        }
        return false;
    };

    QImage image = overlayImage();
    keepFrame(image, "sketch-overlay-free");
    // Free to move: the top and right sides. Held: the two sides that start at the origin can
    // still grow, so every side is free until the sizes are given.
    QVERIFY(hasColor(image, {40.0, 50.0}, colors.sketchFree));
    QVERIFY(hasColor(image, {80.0, 25.0}, colors.sketchFree));
    QVERIFY(!hasColor(image, {40.0, 50.0}, colors.sketchConstrained));
    // Nothing is drawn where there is no geometry.
    QVERIFY(qAlpha(image.pixel(
                (editor()->toWidget({110.0, 70.0}) * image.devicePixelRatio()).toPoint())) == 0);

    // Give it its sizes: it turns to the color of fully constrained geometry.
    Sketch sized = sketch();
    sized.addConstraint(ConstraintKind::Distance, lines[0].start, lines[0].end, 0, 80.0);
    sized.addConstraint(ConstraintKind::Distance, lines[1].start, lines[1].end, 0, 50.0);
    sized.solve();
    m_document->setSketchGeometry(m_sketch, sized, "Sizes");
    QVERIFY(editor()->state().isFullyConstrained());
    image = overlayImage();
    keepFrame(image, "sketch-overlay-constrained");
    QVERIFY(hasColor(image, {40.0, 50.0}, colors.sketchConstrained));
    QVERIFY(hasColor(image, {80.0, 25.0}, colors.sketchConstrained));
    QVERIFY(!hasColor(image, {40.0, 50.0}, colors.sketchFree));

    // What is selected is drawn in the selection color, and what the pointer is over in the
    // hover color.
    editor()->setSelection({lines[2].id});
    image = overlayImage();
    QVERIFY(hasColor(image, {40.0, 50.0}, colors.selection));
    editor()->clearSelection();
    move(at(80.0, 25.0));
    image = overlayImage();
    QVERIFY(hasColor(image, {80.0, 20.0}, colors.hover));

    // The sketch is drawn over the 3D view, in one picture with it.
    move(at(110.0, 70.0));
    const QImage whole = m_viewport->grab().toImage();
    keepFrame(whole, "sketch-over-view");
    const QPoint onTop = (editor()->toWidget({40.0, 50.0}) * whole.devicePixelRatio()).toPoint();
    bool seen = false;
    for (int dy = -4; dy <= 4 && !seen; ++dy) {
        seen = isNear(whole.pixel(onTop + QPoint(0, dy)), colors.sketchConstrained, 40);
    }
    QVERIFY(seen);
}

QTEST_MAIN(SketchViewTest)
#include "SketchViewTest.moc"
