#include <lcad/kernel/Primitives.h>
#include <lcad/viewport/ViewportWidget.h>

#include <FrameChecks.h>

#include <QRegularExpression>
#include <QTest>

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

private:
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

QTEST_MAIN(ViewportTest)
#include "ViewportTest.moc"
