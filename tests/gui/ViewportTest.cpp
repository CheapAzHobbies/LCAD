// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Primitives.h>
#include <lcad/viewport/ViewportWidget.h>

#include <FrameChecks.h>

#include <QApplication>
#include <QRegularExpression>
#include <QTest>
#include <QWheelEvent>

using lcad::test::differingFraction;
using lcad::test::drawnFraction;
using lcad::test::isNear;
using lcad::test::keepFrame;
using lcad::viewport::ViewportWidget;

// These tests only establish that the rendering path works: OCCT adopts Qt's OpenGL context and
// its frame reaches the widget. They say nothing about whether geometry is correct.
class ViewportTest : public QObject {
    Q_OBJECT

public:
    static void initMain() { lcad::viewport::prepareApplication(); }

private slots:
    void init();
    void cleanup();

    void startsWithoutError();
    void emptyViewportShowsTheBackground();
    void displayedShapeIsDrawn();
    void clearingTheShapeEmptiesTheView();

    void hoveringOverTheShapeHighlightsIt();
    void draggingWithTheLeftButtonOrbits();
    void draggingWithTheMiddleButtonPans();
    void turningTheWheelZooms();

private:
    // Shows a box and returns the frame once it is drawn.
    QImage showBox();
    // Presses `button` at `from`, moves to `to`, and releases, drawing a frame at each step as
    // the event loop would.
    void drag(Qt::MouseButton button, QPoint from, QPoint to);

    ViewportWidget* m_viewport = nullptr;
};

void ViewportTest::init() {
    // Qt reports trouble with OpenGL contexts and surfaces as warnings and carries on.
    QTest::failOnWarning(QRegularExpression(QStringLiteral("OpenGL|QRhi|context|surface"),
                                            QRegularExpression::CaseInsensitiveOption));
    m_viewport = new ViewportWidget;
    m_viewport->resize(640, 480);
    m_viewport->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_viewport));
}

void ViewportTest::cleanup() {
    delete m_viewport;
    m_viewport = nullptr;
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

    // The top-right corner is clear of the axis marker in the bottom-left.
    QVERIFY(isNear(frame.pixel(frame.width() - 1, 0), m_viewport->backgroundColor()));
    // Only the axis marker is drawn.
    QVERIFY(drawnFraction(frame, m_viewport->backgroundColor()) < 0.03);
}

void ViewportTest::displayedShapeIsDrawn() {
    m_viewport->setShape(lcad::kernel::makeBox(100.0, 60.0, 40.0));
    const QImage frame = m_viewport->grabFramebuffer();
    keepFrame(frame, "viewport-box");

    // The box is framed with a margin, so it fills a good part of the view but not all of it.
    const double drawn = drawnFraction(frame, m_viewport->backgroundColor());
    QVERIFY2(drawn > 0.10, qPrintable(QStringLiteral("drawn fraction %1").arg(drawn)));
    QVERIFY2(drawn < 0.95, qPrintable(QStringLiteral("drawn fraction %1").arg(drawn)));
    QVERIFY(isNear(frame.pixel(frame.width() - 1, 0), m_viewport->backgroundColor()));
}

void ViewportTest::clearingTheShapeEmptiesTheView() {
    m_viewport->setShape(lcad::kernel::makeBox(100.0, 60.0, 40.0));
    m_viewport->grabFramebuffer();

    m_viewport->setShape(lcad::kernel::Shape());
    const QImage frame = m_viewport->grabFramebuffer();

    QVERIFY(drawnFraction(frame, m_viewport->backgroundColor()) < 0.03);
}

QImage ViewportTest::showBox() {
    m_viewport->setShape(lcad::kernel::makeBox(100.0, 60.0, 40.0));
    return m_viewport->grabFramebuffer();
}

void ViewportTest::drag(Qt::MouseButton button, QPoint from, QPoint to) {
    QTest::mousePress(m_viewport, button, Qt::NoModifier, from);
    m_viewport->grabFramebuffer();
    QTest::mouseMove(m_viewport, (from + to) / 2);
    m_viewport->grabFramebuffer();
    QTest::mouseMove(m_viewport, to);
    m_viewport->grabFramebuffer();
    QTest::mouseRelease(m_viewport, button, Qt::NoModifier, to);
}

// The pointer position has to reach OCCT's picking in the right coordinates for this to work,
// so it also covers the conversion from Qt's logical pixels.
void ViewportTest::hoveringOverTheShapeHighlightsIt() {
    const QImage plain = showBox();
    const QPoint overTheBox = m_viewport->rect().center();
    const QPoint overNothing(m_viewport->width() - 5, 5);

    QTest::mouseMove(m_viewport, overTheBox);
    const QImage hovered = m_viewport->grabFramebuffer();
    keepFrame(hovered, "viewport-box-hovered");
    QTest::mouseMove(m_viewport, overNothing);
    const QImage left = m_viewport->grabFramebuffer();

    QVERIFY(differingFraction(plain, hovered) > 0.001);
    QVERIFY(differingFraction(plain, left) < 0.001);
}

void ViewportTest::draggingWithTheLeftButtonOrbits() {
    const QImage before = showBox();

    drag(Qt::LeftButton, QPoint(320, 240), QPoint(420, 270));
    QTest::mouseMove(m_viewport, QPoint(m_viewport->width() - 5, 5));
    const QImage after = m_viewport->grabFramebuffer();
    keepFrame(after, "viewport-box-orbited");

    QVERIFY(differingFraction(before, after) > 0.05);
    // Orbiting turns the box in place; it stays in view.
    QVERIFY(drawnFraction(after, m_viewport->backgroundColor()) > 0.10);
}

void ViewportTest::draggingWithTheMiddleButtonPans() {
    const QImage before = showBox();
    const QColor background = m_viewport->backgroundColor();
    // Before panning, the right edge of the view is empty margin.
    QVERIFY(isNear(before.pixel(before.width() - 2, before.height() / 2), background));

    drag(Qt::MiddleButton, QPoint(320, 240), QPoint(520, 240));
    QTest::mouseMove(m_viewport, QPoint(m_viewport->width() - 5, 5));
    const QImage after = m_viewport->grabFramebuffer();
    keepFrame(after, "viewport-box-panned");

    // The box followed the pointer 200 pixels to the right and now runs off that edge.
    QVERIFY(!isNear(after.pixel(after.width() - 2, after.height() / 2), background));
}

void ViewportTest::turningTheWheelZooms() {
    const QImage before = showBox();
    const QColor background = m_viewport->backgroundColor();
    const QPointF center = m_viewport->rect().center();

    // Three notches away from the user.
    QWheelEvent wheel(center, m_viewport->mapToGlobal(center), QPoint(), QPoint(0, 360),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(m_viewport, &wheel);
    QTest::mouseMove(m_viewport, QPoint(m_viewport->width() - 5, 5));
    const QImage after = m_viewport->grabFramebuffer();
    keepFrame(after, "viewport-box-zoomed");

    // Rolling the wheel forward zooms in: the box covers more of the view.
    QVERIFY(drawnFraction(after, background) > drawnFraction(before, background) * 1.2);
}

QTEST_MAIN(ViewportTest)
#include "ViewportTest.moc"
