// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/document/Document.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/kernel/Transform.h>
#include <lcad/viewport/ViewportWidget.h>

#include <FrameChecks.h>
#include <GraphicsMessages.h>

#include <QApplication>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>
#include <QWheelEvent>

#include <memory>

using lcad::document::Document;
using lcad::document::ObjectId;
using lcad::kernel::SubShapeKind;
using lcad::test::differingFraction;
using lcad::test::drawnFraction;
using lcad::test::isNear;
using lcad::test::keepFrame;
using lcad::viewport::CameraState;
using lcad::viewport::DisplayStyle;
using lcad::viewport::NavigationPreset;
using lcad::viewport::OriginItem;
using lcad::viewport::Projection;
using lcad::viewport::SelectedItem;
using lcad::viewport::SelectionFilter;
using lcad::viewport::StandardView;
using lcad::viewport::ViewportColors;
using lcad::viewport::ViewportWidget;

namespace {

// Rendering tests compare pictures coarsely; these compare the camera exactly enough.
bool sameDirection(const QVector3D& a, const QVector3D& b) {
    return (a - b).length() < 1e-3F;
}

} // namespace

// The frame checks here establish that the rendering and input paths work. Whether geometry is
// correct is never judged from a picture: the selection tests measure what was picked.
class ViewportTest : public QObject {
    Q_OBJECT

public:
    static void initMain() { lcad::viewport::prepareApplication(); }

private slots:
    void init();
    void cleanup();

    void startsWithoutError();
    void emptyViewportShowsTheBackground();

    // Following the document
    void bodiesOfTheDocumentAreDrawn();
    void changesToTheDocumentAreFollowed();
    void anotherDocumentReplacesTheFirst();
    void previewIsDrawnAndCannotBePicked();

    // The camera
    void standardViewsLookAlongTheirAxes();
    void projectionSwitchesBetweenOrthographicAndPerspective();
    void clickingTheViewCubeTurnsTheCamera();
    void draggingTheViewCubeOrbits();
    void viewCubeCanTurnWithoutAnimation();
    void navigationPresetSwapsPanAndOrbit();
    void fitSelectionFramesWhatIsSelected();
    void cameraStateCanBePutBack();

    // What is drawn besides the document
    void colorsCanBeReplaced();
    void displayStylesChangeHowBodiesAreDrawn();
    void gridAndAxesCanBeSwitched();
    void originPlanesAndAxesCanBeShown();
    void middleButtonPans();
    void shiftMiddleButtonOrbits();
    void ctrlShiftMiddleButtonZooms();
    void wheelZoomsAndCanBeReversed();
    void middleDoubleClickFitsTheView();
    void leftButtonDoesNotMoveTheCamera();

    // Selection
    void hoveringHighlightsWhatIsUnderThePointer();
    void clickingSelectsTheFaceEdgeOrVertexUnderThePointer();
    void shiftClickAddsAndClickingNothingClears();
    void filtersRestrictWhatCanBePicked();
    void draggingARectangleSelectsWhatIsInside();
    void bodiesCanBeSelectedFromOutside();
    void selectionFollowsTheDocument();
    void everythingUnderThePointerCanBeListedAndSelected();

private:
    // Adds the 100 x 60 x 40 box and returns its id, with a frame drawn.
    ObjectId addBox();
    void render() { m_viewport->grabFramebuffer(); }
    // Moves to `position` and clicks there, drawing frames as the event loop would.
    void click(const QPointF& position, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    void drag(Qt::MouseButton button, Qt::KeyboardModifiers modifiers, QPoint from, QPoint to);
    [[nodiscard]] QPoint emptySpot() const { return {5, m_viewport->height() / 2}; }

    std::unique_ptr<Document> m_document;
    ViewportWidget* m_viewport = nullptr;
    lcad::test::GraphicsMessages m_graphicsMessages;
};

void ViewportTest::init() {
    // Qt reports trouble with OpenGL contexts and surfaces as warnings and carries on.
    QTest::failOnWarning(QRegularExpression(QStringLiteral("OpenGL|QRhi|context|surface"),
                                            QRegularExpression::CaseInsensitiveOption));
    m_document = std::make_unique<Document>();
    m_viewport = new ViewportWidget;
    m_viewport->resize(640, 480);
    m_viewport->setDocument(m_document.get());
    m_viewport->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_viewport));
}

void ViewportTest::cleanup() {
    delete m_viewport;
    m_viewport = nullptr;
    m_document.reset();
    const QStringList complaints = m_graphicsMessages.take();
    QVERIFY2(complaints.isEmpty(), qPrintable(complaints.join(QStringLiteral(" | "))));
}

ObjectId ViewportTest::addBox() {
    const ObjectId id = m_document->addBody("Box", lcad::kernel::makeBox(100.0, 60.0, 40.0));
    render();
    return id;
}

void ViewportTest::click(const QPointF& position, Qt::KeyboardModifiers modifiers) {
    const QPoint point = position.toPoint();
    QTest::mouseMove(m_viewport, point);
    render();
    QTest::mouseClick(m_viewport, Qt::LeftButton, modifiers, point);
    render();
}

void ViewportTest::drag(Qt::MouseButton button, Qt::KeyboardModifiers modifiers, QPoint from,
                        QPoint to) {
    QTest::mouseMove(m_viewport, from);
    render();
    QTest::mousePress(m_viewport, button, modifiers, from);
    render();
    // QTest::mouseMove carries no button state, so build the moves by hand.
    for (const QPoint& point : {(from + to) / 2, to}) {
        QMouseEvent move(QEvent::MouseMove, point, m_viewport->mapToGlobal(point), Qt::NoButton,
                         button, modifiers);
        QApplication::sendEvent(m_viewport, &move);
        render();
    }
    QTest::mouseRelease(m_viewport, button, modifiers, to);
    render();
}

void ViewportTest::startsWithoutError() {
    const QImage frame = m_viewport->grabFramebuffer();

    QVERIFY(!frame.isNull());
    QVERIFY2(m_viewport->errorString().isEmpty(), qPrintable(m_viewport->errorString()));
    QVERIFY(!m_viewport->graphicsInfo().isEmpty());
    // Recorded in the test log so a failure report shows which driver was in use.
    qInfo().noquote() << m_viewport->graphicsInfo();
}

void ViewportTest::emptyViewportShowsTheBackground() {
    const QImage frame = m_viewport->grabFramebuffer();
    keepFrame(frame, "viewport-empty");

    // The top-left corner is clear of the axis marker (bottom left) and the view cube (top
    // right), which are all that is drawn.
    QVERIFY(isNear(frame.pixel(0, 0), m_viewport->backgroundColor()));
    QVERIFY(
        isNear(frame.pixel(frame.width() / 2, frame.height() / 2), m_viewport->backgroundColor()));
    const double drawn = drawnFraction(frame, m_viewport->backgroundColor());
    QVERIFY2(drawn > 0.005, qPrintable(QStringLiteral("drawn fraction %1").arg(drawn)));
    QVERIFY2(drawn < 0.08, qPrintable(QStringLiteral("drawn fraction %1").arg(drawn)));
}

void ViewportTest::bodiesOfTheDocumentAreDrawn() {
    const double empty =
        drawnFraction(m_viewport->grabFramebuffer(), m_viewport->backgroundColor());

    addBox();
    const QImage frame = m_viewport->grabFramebuffer();
    keepFrame(frame, "viewport-box");

    // The first body is framed with a margin: it fills a good part of the view, not all of it.
    const double drawn = drawnFraction(frame, m_viewport->backgroundColor());
    QVERIFY2(drawn > empty + 0.10, qPrintable(QStringLiteral("drawn fraction %1").arg(drawn)));
    QVERIFY2(drawn < 0.95, qPrintable(QStringLiteral("drawn fraction %1").arg(drawn)));
    QVERIFY(isNear(frame.pixel(0, 0), m_viewport->backgroundColor()));
}

void ViewportTest::changesToTheDocumentAreFollowed() {
    const QColor background = m_viewport->backgroundColor();
    const double empty = drawnFraction(m_viewport->grabFramebuffer(), background);
    const ObjectId box = addBox();
    const QImage shown = m_viewport->grabFramebuffer();
    QVERIFY(drawnFraction(shown, background) > empty + 0.10);

    m_document->setBodyVisible(box, false);
    QVERIFY(drawnFraction(m_viewport->grabFramebuffer(), background) < empty + 0.005);

    m_document->setBodyVisible(box, true);
    QVERIFY(differingFraction(shown, m_viewport->grabFramebuffer()) < 0.001);

    m_document->removeBody(box);
    QVERIFY(drawnFraction(m_viewport->grabFramebuffer(), background) < empty + 0.005);

    m_document->undo();
    QVERIFY(differingFraction(shown, m_viewport->grabFramebuffer()) < 0.001);

    // A second body does not move the camera: the first one stays exactly where it was drawn.
    m_document->addBody(
        "Far", lcad::kernel::translated(lcad::kernel::makeBox(1.0, 1.0, 1.0), 5000.0, 0.0, 0.0));
    QVERIFY(differingFraction(shown, m_viewport->grabFramebuffer()) < 0.001);
}

void ViewportTest::anotherDocumentReplacesTheFirst() {
    const QColor background = m_viewport->backgroundColor();
    const double empty = drawnFraction(m_viewport->grabFramebuffer(), background);
    addBox();

    Document other;
    m_viewport->setDocument(&other);
    QCOMPARE(m_viewport->document(), &other);
    QVERIFY(drawnFraction(m_viewport->grabFramebuffer(), background) < empty + 0.005);

    // The first document is no longer listened to.
    m_document->addBody("Ignored", lcad::kernel::makeBox(10.0, 10.0, 10.0));
    QVERIFY(drawnFraction(m_viewport->grabFramebuffer(), background) < empty + 0.005);

    other.addBody("Ball", lcad::kernel::makeSphere(20.0));
    QVERIFY(drawnFraction(m_viewport->grabFramebuffer(), background) > empty + 0.10);

    m_viewport->setDocument(nullptr);
    QVERIFY(drawnFraction(m_viewport->grabFramebuffer(), background) < empty + 0.005);
}

void ViewportTest::previewIsDrawnAndCannotBePicked() {
    const QColor background = m_viewport->backgroundColor();
    const double empty = drawnFraction(m_viewport->grabFramebuffer(), background);

    m_viewport->setPreview(lcad::kernel::makeBox(100.0, 60.0, 40.0));
    m_viewport->setStandardView(StandardView::Top);
    const QImage previewed = m_viewport->grabFramebuffer();
    keepFrame(previewed, "viewport-preview");
    QVERIFY(drawnFraction(previewed, background) > empty + 0.10);

    click(m_viewport->project(50.0, 30.0, 40.0));
    QVERIFY(m_viewport->selection().empty());
    QVERIFY(m_document->bodies().empty());

    m_viewport->setPreview(lcad::kernel::Shape());
    QVERIFY(drawnFraction(m_viewport->grabFramebuffer(), background) < empty + 0.005);
}

void ViewportTest::standardViewsLookAlongTheirAxes() {
    addBox();
    QCOMPARE(m_viewport->projection(), Projection::Orthographic);

    const std::pair<StandardView, QVector3D> views[] = {
        {StandardView::Front, {0.0F, 1.0F, 0.0F}},  {StandardView::Back, {0.0F, -1.0F, 0.0F}},
        {StandardView::Right, {-1.0F, 0.0F, 0.0F}}, {StandardView::Left, {1.0F, 0.0F, 0.0F}},
        {StandardView::Top, {0.0F, 0.0F, -1.0F}},   {StandardView::Bottom, {0.0F, 0.0F, 1.0F}},
    };
    for (const auto& [view, direction] : views) {
        m_viewport->setStandardView(view);
        render();
        QVERIFY2(sameDirection(m_viewport->viewDirection(), direction),
                 qPrintable(QStringLiteral("view %1").arg(static_cast<int>(view))));
        // The box is framed in every view.
        const double drawn =
            drawnFraction(m_viewport->grabFramebuffer(), m_viewport->backgroundColor());
        QVERIFY(drawn > 0.10);
    }

    // In the front view X runs to the right and Z up.
    m_viewport->setStandardView(StandardView::Front);
    render();
    QVERIFY(m_viewport->project(100.0, 0.0, 0.0).x() > m_viewport->project(0.0, 0.0, 0.0).x());
    QVERIFY(m_viewport->project(0.0, 0.0, 40.0).y() < m_viewport->project(0.0, 0.0, 0.0).y());

    m_viewport->setStandardView(StandardView::Home);
    render();
    const QVector3D home = m_viewport->viewDirection();
    // From the front, right, and top: looking toward -X, +Y, and -Z.
    QVERIFY(home.x() < -0.5F && home.y() > 0.5F && home.z() < -0.5F);
}

void ViewportTest::projectionSwitchesBetweenOrthographicAndPerspective() {
    addBox();
    const QImage orthographic = m_viewport->grabFramebuffer();

    m_viewport->setProjection(Projection::Perspective);
    const QImage perspective = m_viewport->grabFramebuffer();
    keepFrame(perspective, "viewport-box-perspective");
    QCOMPARE(m_viewport->projection(), Projection::Perspective);
    // Perspective foreshortens the box, so the picture changes; the box stays in view.
    QVERIFY(differingFraction(orthographic, perspective) > 0.01);
    QVERIFY(drawnFraction(perspective, m_viewport->backgroundColor()) > 0.10);

    m_viewport->setProjection(Projection::Orthographic);
    QCOMPARE(m_viewport->projection(), Projection::Orthographic);
    QVERIFY(differingFraction(orthographic, m_viewport->grabFramebuffer()) < 0.001);
}

void ViewportTest::clickingTheViewCubeTurnsTheCamera() {
    addBox();
    const QVector3D home = m_viewport->viewDirection();
    keepFrame(m_viewport->grabFramebuffer(), "viewport-view-cube");

    // In the home view the cube shows its top, front, and right sides around its near corner.
    // The front side is below and to the left of that corner.
    click(m_viewport->viewCubeCenter() + QPointF(-14.0, 14.0));

    // The camera turns in an animation.
    QTRY_VERIFY_WITH_TIMEOUT(sameDirection(m_viewport->viewDirection(), {0.0F, 1.0F, 0.0F}), 5000);
    QVERIFY(!sameDirection(home, m_viewport->viewDirection()));
    // Clicking the cube is navigation, not selection.
    QVERIFY(m_viewport->selection().empty());
}

void ViewportTest::draggingTheViewCubeOrbits() {
    addBox();
    const QVector3D home = m_viewport->viewDirection();
    const QPointF origin = m_viewport->project(50.0, 30.0, 20.0);
    const QPoint onTheCube = m_viewport->viewCubeCenter().toPoint();

    // Hold the left button on the cube and move: the camera turns with the pointer.
    drag(Qt::LeftButton, Qt::NoModifier, onTheCube, onTheCube + QPoint(-90, 30));
    QTest::mouseMove(m_viewport, emptySpot());
    const QImage after = m_viewport->grabFramebuffer();
    keepFrame(after, "viewport-cube-dragged");

    const QVector3D turned = m_viewport->viewDirection();
    QVERIFY(!sameDirection(turned, home));
    // A free orbit, not a jump to one of the cube's sides.
    for (const QVector3D& axis : {QVector3D(1, 0, 0), QVector3D(0, 1, 0), QVector3D(0, 0, 1)}) {
        QVERIFY(qAbs(QVector3D::dotProduct(turned, axis)) < 0.999F);
    }
    // The model turns about its middle, which stays where it was on screen, and stays in view.
    const QPointF middle = m_viewport->project(50.0, 30.0, 20.0);
    QVERIFY2(QLineF(origin, middle).length() < 3.0, qPrintable(QString::number(middle.x())));
    QVERIFY(drawnFraction(after, m_viewport->backgroundColor()) > 0.10);
    QVERIFY(m_viewport->selection().empty());

    // Dragging further keeps turning, and a plain click on the cube still jumps to a side.
    drag(Qt::LeftButton, Qt::NoModifier, onTheCube, onTheCube + QPoint(40, -60));
    QVERIFY(!sameDirection(m_viewport->viewDirection(), turned));
    m_viewport->setStandardView(StandardView::Home);
    render();
    click(m_viewport->viewCubeCenter() + QPointF(-14.0, 14.0));
    QTRY_VERIFY_WITH_TIMEOUT(sameDirection(m_viewport->viewDirection(), {0.0F, 1.0F, 0.0F}), 5000);
}

void ViewportTest::viewCubeCanTurnWithoutAnimation() {
    addBox();
    QVERIFY(m_viewport->isAnimated());
    m_viewport->setAnimated(false);

    click(m_viewport->viewCubeCenter() + QPointF(-14.0, 14.0));
    // No waiting: a frame or two later the camera is there.
    render();
    render();

    QVERIFY(sameDirection(m_viewport->viewDirection(), {0.0F, 1.0F, 0.0F}));
}

void ViewportTest::navigationPresetSwapsPanAndOrbit() {
    addBox();
    QCOMPARE(m_viewport->navigationPreset(), NavigationPreset::MiddlePans);
    m_viewport->setNavigationPreset(NavigationPreset::MiddleOrbits);
    const QVector3D direction = m_viewport->viewDirection();

    // The middle button alone now orbits.
    drag(Qt::MiddleButton, Qt::NoModifier, QPoint(320, 240), QPoint(420, 270));
    const QVector3D orbited = m_viewport->viewDirection();
    QVERIFY(!sameDirection(orbited, direction));

    // With Shift it pans: the model follows the pointer and the camera keeps facing as it did.
    const QPointF before = m_viewport->project(0.0, 0.0, 0.0);
    drag(Qt::MiddleButton, Qt::ShiftModifier, QPoint(320, 240), QPoint(420, 200));
    const QPointF after = m_viewport->project(0.0, 0.0, 0.0);
    QVERIFY(qAbs(after.x() - before.x() - 100.0) < 3.0);
    QVERIFY(sameDirection(m_viewport->viewDirection(), orbited));

    // And dragging the view cube still orbits.
    const QPoint onTheCube = m_viewport->viewCubeCenter().toPoint();
    QTest::mouseMove(m_viewport, onTheCube);
    render();
    render();
    drag(Qt::LeftButton, Qt::NoModifier, onTheCube, onTheCube + QPoint(-70, 20));
    QVERIFY(!sameDirection(m_viewport->viewDirection(), orbited));
}

void ViewportTest::fitSelectionFramesWhatIsSelected() {
    addBox();
    const ObjectId peg = m_document->addBody(
        "Peg", lcad::kernel::translated(lcad::kernel::makeBox(4.0, 4.0, 4.0), 300.0, 0.0, 0.0));
    m_viewport->fitAll();
    render();
    const auto pegWidth = [this] {
        return QLineF(m_viewport->project(300.0, 0.0, 0.0), m_viewport->project(304.0, 0.0, 0.0))
            .length();
    };
    const double small = pegWidth();

    m_viewport->setSelectedBodies({peg});
    m_viewport->fitSelection();
    render();

    // The 4 mm peg now fills a good part of the view, and sits in its middle.
    QVERIFY2(pegWidth() > small * 10.0,
             qPrintable(QStringLiteral("%1 -> %2").arg(small).arg(pegWidth())));
    const QPointF center = m_viewport->project(302.0, 2.0, 2.0);
    QVERIFY(QLineF(center, QPointF(m_viewport->rect().center())).length() < 20.0);

    // With nothing selected it frames everything again.
    m_viewport->clearSelection();
    m_viewport->fitSelection();
    render();
    QVERIFY(qAbs(pegWidth() - small) < small * 0.05);
}

void ViewportTest::cameraStateCanBePutBack() {
    addBox();
    m_viewport->setProjection(Projection::Perspective);
    drag(Qt::MiddleButton, Qt::ShiftModifier, QPoint(320, 240), QPoint(380, 300));
    drag(Qt::MiddleButton, Qt::NoModifier, QPoint(320, 240), QPoint(250, 260));
    QTest::mouseMove(m_viewport, emptySpot());
    const QImage arranged = m_viewport->grabFramebuffer();
    const CameraState state = m_viewport->cameraState();

    m_viewport->setProjection(Projection::Orthographic);
    m_viewport->setStandardView(StandardView::Top);
    QVERIFY(differingFraction(arranged, m_viewport->grabFramebuffer()) > 0.05);

    m_viewport->setCameraState(state);
    QCOMPARE(m_viewport->projection(), Projection::Perspective);
    QVERIFY(differingFraction(arranged, m_viewport->grabFramebuffer()) < 0.001);
}

void ViewportTest::colorsCanBeReplaced() {
    addBox();
    const QImage dark = m_viewport->grabFramebuffer();
    ViewportColors light = m_viewport->colors();
    light.background = QColor(236, 238, 241);
    light.body = QColor(150, 160, 172);
    light.line = QColor(30, 34, 40);

    m_viewport->setColors(light);
    const QImage frame = m_viewport->grabFramebuffer();
    keepFrame(frame, "viewport-box-light");

    QCOMPARE(m_viewport->colors(), light);
    QCOMPARE(m_viewport->backgroundColor(), light.background);
    QVERIFY(isNear(frame.pixel(0, 0), light.background));
    // The box is drawn against the new background, and differs from it.
    const double drawn = drawnFraction(frame, light.background);
    QVERIFY2(drawn > 0.10 && drawn < 0.95, qPrintable(QString::number(drawn)));
    QVERIFY(differingFraction(dark, frame) > 0.5);
}

void ViewportTest::displayStylesChangeHowBodiesAreDrawn() {
    m_document->addBody("Ball", lcad::kernel::makeSphere(40.0));
    const QColor background = m_viewport->backgroundColor();
    QCOMPARE(m_viewport->displayStyle(), DisplayStyle::ShadedWithEdges);
    const QImage withEdges = m_viewport->grabFramebuffer();
    const double solid = drawnFraction(withEdges, background);
    QVERIFY(solid > 0.10);

    // Wireframe: a few lines where the filled ball was.
    m_viewport->setDisplayStyle(DisplayStyle::Wireframe);
    const QImage wireframe = m_viewport->grabFramebuffer();
    keepFrame(wireframe, "viewport-ball-wireframe");
    const double wires = drawnFraction(wireframe, background);
    QVERIFY2(wires > 0.001 && wires < solid * 0.3, qPrintable(QString::number(wires)));

    m_viewport->setDisplayStyle(DisplayStyle::HiddenLine);
    const QImage hiddenLine = m_viewport->grabFramebuffer();
    keepFrame(hiddenLine, "viewport-ball-hidden-line");
    const double visibleLines = drawnFraction(hiddenLine, background);
    QVERIFY2(visibleLines > 0.001 && visibleLines < solid * 0.3,
             qPrintable(QString::number(visibleLines)));
    // Not the same lines as the wireframe: the ball's outline as seen from here, which the
    // wireframe does not have, and none of the seam that runs around its back.
    QVERIFY(differingFraction(wireframe, hiddenLine) > 0.002);

    // Shaded without edges: as filled as before, minus the outline.
    m_viewport->setDisplayStyle(DisplayStyle::Shaded);
    const QImage shaded = m_viewport->grabFramebuffer();
    QVERIFY(qAbs(drawnFraction(shaded, background) - solid) < 0.02);
    QVERIFY(differingFraction(withEdges, shaded) > 0.0005);

    // Transparent: the same outline, with the background showing through.
    m_viewport->setDisplayStyle(DisplayStyle::Transparent);
    const QImage transparent = m_viewport->grabFramebuffer();
    keepFrame(transparent, "viewport-ball-transparent");
    QVERIFY(differingFraction(withEdges, transparent) > 0.10);
    QVERIFY(drawnFraction(transparent, background) > solid * 0.8);

    m_viewport->setDisplayStyle(DisplayStyle::ShadedWithEdges);
    QVERIFY(differingFraction(withEdges, m_viewport->grabFramebuffer()) < 0.001);

    // A body added later is drawn in the style that is set.
    m_viewport->setDisplayStyle(DisplayStyle::Wireframe);
    m_document->removeBody(m_document->bodies()[0].id);
    m_document->addBody("Ball", lcad::kernel::makeSphere(40.0));
    QVERIFY(drawnFraction(m_viewport->grabFramebuffer(), background) < solid * 0.3);
}

void ViewportTest::gridAndAxesCanBeSwitched() {
    const QColor background = m_viewport->backgroundColor();
    QVERIFY(!m_viewport->isGridVisible());
    QVERIFY(m_viewport->areAxesVisible());
    const QImage plain = m_viewport->grabFramebuffer();
    const double withAxes = drawnFraction(plain, background);

    m_viewport->setAxesVisible(false);
    QVERIFY(drawnFraction(m_viewport->grabFramebuffer(), background) < withAxes - 0.0005);
    m_viewport->setAxesVisible(true);
    QVERIFY(differingFraction(plain, m_viewport->grabFramebuffer()) < 0.001);

    addBox();
    const QImage box = m_viewport->grabFramebuffer();
    const double boxLength =
        QLineF(m_viewport->project(0.0, 0.0, 0.0), m_viewport->project(100.0, 0.0, 0.0)).length();
    m_viewport->setGridVisible(true);
    QVERIFY(m_viewport->isGridVisible());
    const QImage gridded = m_viewport->grabFramebuffer();
    keepFrame(gridded, "viewport-box-grid");
    // Grid lines appear around the box.
    QVERIFY(differingFraction(box, gridded) > 0.01);

    // The grid is a guide, not part of the model: fitting still frames the box.
    m_viewport->fitAll();
    render();
    const double fitted =
        QLineF(m_viewport->project(0.0, 0.0, 0.0), m_viewport->project(100.0, 0.0, 0.0)).length();
    QVERIFY2(qAbs(fitted - boxLength) < boxLength * 0.02,
             qPrintable(QStringLiteral("%1 -> %2").arg(boxLength).arg(fitted)));

    m_viewport->setGridVisible(false);
    QVERIFY(differingFraction(box, m_viewport->grabFramebuffer()) < 0.001);
}

void ViewportTest::originPlanesAndAxesCanBeShown() {
    // Two bodies on either side of the origin, so that it is in view and nothing covers it.
    m_document->addBody("Near", lcad::kernel::translated(lcad::kernel::makeBox(40.0, 40.0, 20.0),
                                                         -160.0, -120.0, 0.0));
    m_document->addBody(
        "Far", lcad::kernel::translated(lcad::kernel::makeBox(40.0, 40.0, 20.0), 120.0, 90.0, 0.0));
    m_viewport->fitAll();
    render();
    QTest::mouseMove(m_viewport, emptySpot());
    const QImage plain = m_viewport->grabFramebuffer();
    const double boxLength =
        QLineF(m_viewport->project(0.0, 0.0, 0.0), m_viewport->project(100.0, 0.0, 0.0)).length();

    for (const OriginItem item : {OriginItem::PlaneXY, OriginItem::PlaneXZ, OriginItem::PlaneYZ,
                                  OriginItem::AxisX, OriginItem::AxisY, OriginItem::AxisZ}) {
        QVERIFY(!m_viewport->isOriginItemVisible(item));
        const QImage before = m_viewport->grabFramebuffer();
        m_viewport->setOriginItemVisible(item, true);
        QVERIFY(m_viewport->isOriginItemVisible(item));
        QVERIFY2(differingFraction(before, m_viewport->grabFramebuffer()) > 0.0003,
                 qPrintable(QStringLiteral("item %1").arg(static_cast<int>(item))));
    }
    keepFrame(m_viewport->grabFramebuffer(), "viewport-origin");

    // They are guides: a click on one selects nothing, and fitting ignores them.
    click(m_viewport->project(0.0, 0.0, 0.0) + QPointF(4.0, -14.0));
    QVERIFY(m_viewport->selection().empty());
    m_viewport->fitAll();
    render();
    const double fitted =
        QLineF(m_viewport->project(0.0, 0.0, 0.0), m_viewport->project(100.0, 0.0, 0.0)).length();
    QVERIFY(qAbs(fitted - boxLength) < boxLength * 0.02);

    for (const OriginItem item : {OriginItem::PlaneXY, OriginItem::PlaneXZ, OriginItem::PlaneYZ,
                                  OriginItem::AxisX, OriginItem::AxisY, OriginItem::AxisZ}) {
        m_viewport->setOriginItemVisible(item, false);
    }
    QTest::mouseMove(m_viewport, emptySpot());
    QVERIFY(differingFraction(plain, m_viewport->grabFramebuffer()) < 0.001);
}

void ViewportTest::middleButtonPans() {
    addBox();
    const QVector3D direction = m_viewport->viewDirection();
    const QPointF before = m_viewport->project(0.0, 0.0, 0.0);

    drag(Qt::MiddleButton, Qt::NoModifier, QPoint(320, 240), QPoint(420, 200));

    // The model follows the pointer: 100 pixels right and 40 up, with the camera facing as it did.
    const QPointF after = m_viewport->project(0.0, 0.0, 0.0);
    QVERIFY2(qAbs(after.x() - before.x() - 100.0) < 3.0, qPrintable(QString::number(after.x())));
    QVERIFY2(qAbs(after.y() - before.y() + 40.0) < 3.0, qPrintable(QString::number(after.y())));
    QVERIFY(sameDirection(m_viewport->viewDirection(), direction));
}

void ViewportTest::shiftMiddleButtonOrbits() {
    addBox();
    const QVector3D before = m_viewport->viewDirection();

    drag(Qt::MiddleButton, Qt::ShiftModifier, QPoint(320, 240), QPoint(420, 270));
    QTest::mouseMove(m_viewport, emptySpot());
    const QImage after = m_viewport->grabFramebuffer();
    keepFrame(after, "viewport-box-orbited");

    QVERIFY(!sameDirection(m_viewport->viewDirection(), before));
    // Orbiting turns the box in place; it stays in view.
    QVERIFY(drawnFraction(after, m_viewport->backgroundColor()) > 0.10);
}

void ViewportTest::ctrlShiftMiddleButtonZooms() {
    addBox();
    const QVector3D direction = m_viewport->viewDirection();
    const double before =
        QLineF(m_viewport->project(0.0, 0.0, 0.0), m_viewport->project(100.0, 0.0, 0.0)).length();

    drag(Qt::MiddleButton, Qt::ControlModifier | Qt::ShiftModifier, QPoint(320, 240),
         QPoint(420, 240));

    const double after =
        QLineF(m_viewport->project(0.0, 0.0, 0.0), m_viewport->project(100.0, 0.0, 0.0)).length();
    QVERIFY2(qAbs(after - before) > before * 0.05,
             qPrintable(QStringLiteral("%1 -> %2").arg(before).arg(after)));
    QVERIFY(sameDirection(m_viewport->viewDirection(), direction));
}

void ViewportTest::wheelZoomsAndCanBeReversed() {
    addBox();
    const auto boxLength = [this] {
        return QLineF(m_viewport->project(0.0, 0.0, 0.0), m_viewport->project(100.0, 0.0, 0.0))
            .length();
    };
    const auto roll = [this](int eighthsOfADegree) {
        const QPointF center = m_viewport->rect().center();
        QWheelEvent wheel(center, m_viewport->mapToGlobal(center), QPoint(),
                          QPoint(0, eighthsOfADegree), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(m_viewport, &wheel);
        render();
    };
    const double start = boxLength();

    // Three notches away from the user zoom in: the box is drawn larger.
    roll(360);
    const double zoomedIn = boxLength();
    QVERIFY2(zoomedIn > start * 1.2,
             qPrintable(QStringLiteral("%1 -> %2").arg(start).arg(zoomedIn)));

    roll(-360);
    QVERIFY(boxLength() < zoomedIn / 1.2);

    m_viewport->setZoomReversed(true);
    QVERIFY(m_viewport->isZoomReversed());
    const double beforeReversed = boxLength();
    roll(360);
    QVERIFY(boxLength() < beforeReversed / 1.2);
}

void ViewportTest::middleDoubleClickFitsTheView() {
    addBox();
    const QImage framed = m_viewport->grabFramebuffer();
    drag(Qt::MiddleButton, Qt::NoModifier, QPoint(320, 240), QPoint(600, 60));
    QTest::mouseMove(m_viewport, emptySpot());
    QVERIFY(differingFraction(framed, m_viewport->grabFramebuffer()) > 0.05);

    QTest::mouseDClick(m_viewport, Qt::MiddleButton, Qt::NoModifier, QPoint(100, 100));
    QTest::mouseMove(m_viewport, emptySpot());

    QVERIFY(differingFraction(framed, m_viewport->grabFramebuffer()) < 0.001);
}

void ViewportTest::leftButtonDoesNotMoveTheCamera() {
    addBox();
    QTest::mouseMove(m_viewport, emptySpot());
    const QImage before = m_viewport->grabFramebuffer();
    const QVector3D direction = m_viewport->viewDirection();
    const QPointF origin = m_viewport->project(0.0, 0.0, 0.0);

    // A drag over empty background, well away from the box.
    drag(Qt::LeftButton, Qt::NoModifier, QPoint(8, 8), QPoint(60, 40));
    QTest::mouseMove(m_viewport, emptySpot());

    QVERIFY(sameDirection(m_viewport->viewDirection(), direction));
    QCOMPARE(m_viewport->project(0.0, 0.0, 0.0), origin);
    QVERIFY(differingFraction(before, m_viewport->grabFramebuffer()) < 0.001);
}

// The pointer position has to reach OCCT's picking in the right coordinates for this to work,
// so it also covers the conversion from Qt's logical pixels.
void ViewportTest::hoveringHighlightsWhatIsUnderThePointer() {
    addBox();
    m_viewport->setStandardView(StandardView::Top);
    QTest::mouseMove(m_viewport, emptySpot());
    const QImage plain = m_viewport->grabFramebuffer();

    QTest::mouseMove(m_viewport, m_viewport->project(50.0, 30.0, 40.0).toPoint());
    // The highlight may take a second frame, which the viewport schedules itself. It is an
    // outline around the top face, so it changes few pixels.
    QTRY_VERIFY(differingFraction(plain, m_viewport->grabFramebuffer()) > 0.002);
    const QImage hovered = m_viewport->grabFramebuffer();
    keepFrame(hovered, "viewport-box-hovered");
    QTest::mouseMove(m_viewport, emptySpot());
    const QImage left = m_viewport->grabFramebuffer();

    QVERIFY(differingFraction(plain, hovered) < 0.05);
    QVERIFY(differingFraction(plain, left) < 0.001);
    // Hovering is not selecting.
    QVERIFY(m_viewport->selection().empty());
}

void ViewportTest::clickingSelectsTheFaceEdgeOrVertexUnderThePointer() {
    const ObjectId box = addBox();
    const lcad::kernel::Shape shape = m_document->findBody(box)->shape;
    m_viewport->setStandardView(StandardView::Top);
    render();
    QSignalSpy changed(m_viewport, &ViewportWidget::selectionChanged);

    // The middle of the top face, which measures 100 mm by 60 mm.
    click(m_viewport->project(50.0, 30.0, 40.0));
    std::vector<SelectedItem> selection = m_viewport->selection();
    QCOMPARE(selection.size(), std::size_t{1});
    QCOMPARE(selection[0].body, box);
    QCOMPARE(selection[0].kind, SelectedItem::Kind::Face);
    const lcad::kernel::Shape face =
        lcad::kernel::subShape(shape, SubShapeKind::Face, selection[0].index);
    QVERIFY(qAbs(lcad::kernel::surfaceArea(face) - 6000.0) < 1e-6);
    QVERIFY(qAbs(lcad::kernel::boundingBox(face)->min.z - 40.0) < 1e-6);
    QCOMPARE(changed.count(), 1);
    keepFrame(m_viewport->grabFramebuffer(), "viewport-face-selected");

    // The middle of the top face's edge along X at Y = 0, which is 100 mm long.
    click(m_viewport->project(50.0, 0.0, 40.0));
    selection = m_viewport->selection();
    QCOMPARE(selection.size(), std::size_t{1});
    QCOMPARE(selection[0].kind, SelectedItem::Kind::Edge);
    const lcad::kernel::Shape edge =
        lcad::kernel::subShape(shape, SubShapeKind::Edge, selection[0].index);
    QVERIFY(qAbs(lcad::kernel::length(edge) - 100.0) < 1e-6);
    QVERIFY(qAbs(lcad::kernel::boundingBox(edge)->max.y) < 1e-6);
    QCOMPARE(changed.count(), 2);

    // The corner at X = 100, Y = 60.
    click(m_viewport->project(100.0, 60.0, 40.0));
    selection = m_viewport->selection();
    QCOMPARE(selection.size(), std::size_t{1});
    QCOMPARE(selection[0].kind, SelectedItem::Kind::Vertex);
    const auto corner = lcad::kernel::boundingBox(
        lcad::kernel::subShape(shape, SubShapeKind::Vertex, selection[0].index));
    QVERIFY(qAbs(corner->min.x - 100.0) < 1e-6);
    QVERIFY(qAbs(corner->min.y - 60.0) < 1e-6);
    QCOMPARE(changed.count(), 3);
}

void ViewportTest::shiftClickAddsAndClickingNothingClears() {
    addBox();
    m_viewport->setStandardView(StandardView::Top);
    render();
    QSignalSpy changed(m_viewport, &ViewportWidget::selectionChanged);

    click(m_viewport->project(50.0, 30.0, 40.0));
    click(m_viewport->project(50.0, 0.0, 40.0), Qt::ShiftModifier);
    click(m_viewport->project(0.0, 30.0, 40.0), Qt::ControlModifier);
    std::vector<SelectedItem> selection = m_viewport->selection();
    QCOMPARE(selection.size(), std::size_t{3});
    QCOMPARE(selection[0].kind, SelectedItem::Kind::Face);
    QCOMPARE(selection[1].kind, SelectedItem::Kind::Edge);
    QCOMPARE(selection[2].kind, SelectedItem::Kind::Edge);
    QVERIFY(selection[1].index != selection[2].index);

    // Shift-clicking something already selected takes it out again.
    click(m_viewport->project(50.0, 0.0, 40.0), Qt::ShiftModifier);
    QCOMPARE(m_viewport->selection().size(), std::size_t{2});
    QCOMPARE(changed.count(), 4);

    click(emptySpot());
    QVERIFY(m_viewport->selection().empty());
    QCOMPARE(changed.count(), 5);
    // Clicking nothing again changes nothing, and says nothing.
    click(emptySpot());
    QCOMPARE(changed.count(), 5);
}

void ViewportTest::filtersRestrictWhatCanBePicked() {
    const ObjectId box = addBox();
    m_viewport->setStandardView(StandardView::Top);
    render();
    const QPointF onTheEdge = m_viewport->project(50.0, 0.0, 40.0);
    const QPointF onTheFace = m_viewport->project(50.0, 30.0, 40.0);
    QCOMPARE(m_viewport->selectionFilter(), SelectionFilter::FacesEdgesVertices);

    m_viewport->setSelectionFilter(SelectionFilter::Bodies);
    click(onTheFace);
    QCOMPARE(m_viewport->selection(),
             (std::vector<SelectedItem>{{box, SelectedItem::Kind::Body, -1}}));

    // Changing the filter drops a selection the new filter could not have made.
    m_viewport->setSelectionFilter(SelectionFilter::Faces);
    QVERIFY(m_viewport->selection().empty());
    // Just inside the edge, where the default filter would pick the edge.
    click(onTheEdge + QPointF(0.0, -2.0));
    QCOMPARE(m_viewport->selection().size(), std::size_t{1});
    QCOMPARE(m_viewport->selection()[0].kind, SelectedItem::Kind::Face);

    m_viewport->setSelectionFilter(SelectionFilter::Edges);
    click(onTheFace);
    QVERIFY(m_viewport->selection().empty());
    click(onTheEdge);
    QCOMPARE(m_viewport->selection().size(), std::size_t{1});
    QCOMPARE(m_viewport->selection()[0].kind, SelectedItem::Kind::Edge);

    m_viewport->setSelectionFilter(SelectionFilter::Vertices);
    click(onTheEdge);
    QVERIFY(m_viewport->selection().empty());
    click(m_viewport->project(0.0, 0.0, 40.0));
    QCOMPARE(m_viewport->selection().size(), std::size_t{1});
    QCOMPARE(m_viewport->selection()[0].kind, SelectedItem::Kind::Vertex);
}

void ViewportTest::draggingARectangleSelectsWhatIsInside() {
    const ObjectId box = addBox();
    m_viewport->setStandardView(StandardView::Top);
    m_viewport->setSelectionFilter(SelectionFilter::Faces);
    render();
    const QPoint outsideNear = m_viewport->project(-10.0, 70.0, 40.0).toPoint();
    const QPoint outsideFar = m_viewport->project(110.0, -10.0, 40.0).toPoint();

    // Around the whole box: all six faces lie inside the rectangle.
    drag(Qt::LeftButton, Qt::NoModifier, outsideNear, outsideFar);
    std::vector<SelectedItem> selection = m_viewport->selection();
    QCOMPARE(selection.size(), std::size_t{6});
    std::vector<int> indices;
    for (const SelectedItem& item : selection) {
        QCOMPARE(item.body, box);
        QCOMPARE(item.kind, SelectedItem::Kind::Face);
        indices.push_back(item.index);
    }
    std::sort(indices.begin(), indices.end());
    QCOMPARE(indices, (std::vector<int>{0, 1, 2, 3, 4, 5}));

    // Around the left half only: the one face that lies wholly inside is the side at X = 0.
    drag(Qt::LeftButton, Qt::NoModifier, outsideNear,
         m_viewport->project(50.0, -10.0, 40.0).toPoint());
    selection = m_viewport->selection();
    QCOMPARE(selection.size(), std::size_t{1});
    const auto bounds = lcad::kernel::boundingBox(lcad::kernel::subShape(
        m_document->findBody(box)->shape, SubShapeKind::Face, selection[0].index));
    QVERIFY(qAbs(bounds->max.x) < 1e-6);
}

void ViewportTest::bodiesCanBeSelectedFromOutside() {
    const ObjectId box = addBox();
    const ObjectId ball = m_document->addBody(
        "Ball", lcad::kernel::translated(lcad::kernel::makeSphere(20.0), 200.0, 0.0, 0.0));
    m_viewport->fitAll();
    QTest::mouseMove(m_viewport, emptySpot());
    const QImage plain = m_viewport->grabFramebuffer();
    QSignalSpy changed(m_viewport, &ViewportWidget::selectionChanged);

    // Works whatever the filter is; the default one picks faces, edges, and vertices.
    m_viewport->setSelectedBodies({ball});
    QCOMPARE(m_viewport->selection(),
             (std::vector<SelectedItem>{{ball, SelectedItem::Kind::Body, -1}}));
    QCOMPARE(changed.count(), 1);
    const QImage ballSelected = m_viewport->grabFramebuffer();
    keepFrame(ballSelected, "viewport-body-selected");
    QVERIFY(differingFraction(plain, ballSelected) > 0.005);

    m_viewport->setSelectedBodies({box, ball, ObjectId{999}});
    QCOMPARE(m_viewport->selection().size(), std::size_t{2});
    QCOMPARE(changed.count(), 2);

    // Setting the same selection again is not a change.
    m_viewport->setSelectedBodies({box, ball});
    QCOMPARE(changed.count(), 2);

    m_viewport->clearSelection();
    QVERIFY(m_viewport->selection().empty());
    QCOMPARE(changed.count(), 3);
    QVERIFY(differingFraction(plain, m_viewport->grabFramebuffer()) < 0.001);
}

void ViewportTest::selectionFollowsTheDocument() {
    const ObjectId box = addBox();
    m_viewport->setStandardView(StandardView::Top);
    render();
    click(m_viewport->project(50.0, 30.0, 40.0));
    QCOMPARE(m_viewport->selection().size(), std::size_t{1});
    QSignalSpy changed(m_viewport, &ViewportWidget::selectionChanged);

    // A face of a body that is gone cannot stay selected.
    m_document->removeBody(box);
    QVERIFY(m_viewport->selection().empty());
    QCOMPARE(changed.count(), 1);

    m_document->undo();
    m_viewport->setSelectedBodies({box});
    QCOMPARE(m_viewport->selection().size(), std::size_t{1});
    m_document->setBodyVisible(box, false);
    QVERIFY(m_viewport->selection().empty());

    // The pointer has not moved since the click on the face, and now there is nothing under
    // it. A click there must see that, and then see the box again once it is back.
    const QPoint where = m_viewport->project(50.0, 30.0, 40.0).toPoint();
    QTest::mouseClick(m_viewport, Qt::LeftButton, Qt::NoModifier, where);
    render();
    QVERIFY(m_viewport->selection().empty());
    m_document->setBodyVisible(box, true);
    render();
    QTest::mouseClick(m_viewport, Qt::LeftButton, Qt::NoModifier, where);
    render();
    QCOMPARE(m_viewport->selection().size(), std::size_t{1});
    QCOMPARE(m_viewport->selection()[0].kind, SelectedItem::Kind::Face);
}

void ViewportTest::everythingUnderThePointerCanBeListedAndSelected() {
    const ObjectId box = addBox();
    const lcad::kernel::Shape shape = m_document->findBody(box)->shape;
    m_viewport->setStandardView(StandardView::Top);
    m_viewport->setSelectionFilter(SelectionFilter::Faces);
    render();
    const QPoint middle = m_viewport->project(50.0, 30.0, 40.0).toPoint();

    // Looking down on the box, the top face is in front and the bottom face behind it.
    const std::vector<SelectedItem> under = m_viewport->itemsAt(middle);
    QCOMPARE(under.size(), std::size_t{2});
    const auto heightOf = [&shape](const SelectedItem& item) {
        return lcad::kernel::boundingBox(
                   lcad::kernel::subShape(shape, SubShapeKind::Face, item.index))
            ->min.z;
    };
    QVERIFY(qAbs(heightOf(under[0]) - 40.0) < 1e-6);
    QVERIFY(qAbs(heightOf(under[1])) < 1e-6);
    QVERIFY(m_viewport->selection().empty());
    QVERIFY(m_viewport->itemsAt(emptySpot()).empty());

    // The one behind can be selected, which no click can do.
    QSignalSpy changed(m_viewport, &ViewportWidget::selectionChanged);
    m_viewport->setSelection({under[1]});
    QCOMPARE(m_viewport->selection(), (std::vector<SelectedItem>{under[1]}));
    QCOMPARE(changed.count(), 1);

    // Whole bodies and several items at once work too; what the filter excludes is ignored.
    m_viewport->setSelection({under[0],
                              under[1],
                              {box, SelectedItem::Kind::Body, -1},
                              {box, SelectedItem::Kind::Edge, 0}});
    QCOMPARE(m_viewport->selection().size(), std::size_t{3});
}

QTEST_MAIN(ViewportTest)
#include "ViewportTest.moc"
