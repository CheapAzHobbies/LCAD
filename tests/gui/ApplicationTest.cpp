#include <lcad/app/Application.h>
#include <lcad/app/MainWindow.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/Version.h>
#include <lcad/viewport/ViewportWidget.h>

#include <FrameChecks.h>

#include <QGuiApplication>
#include <QMenuBar>
#include <QRegularExpression>
#include <QScreen>
#include <QTest>

#include <xcb/xcb.h>

#include <cstdlib>
#include <cstring>

using lcad::app::MainWindow;
using lcad::test::differingFraction;
using lcad::test::drawnFraction;
using lcad::test::keepFrame;

namespace {

// What the X server shows for `widget`, as opposed to what the widget believes it drew.
QImage onScreen(const QWidget* widget) {
    const QWidget* window = widget->window();
    const qreal scale = window->devicePixelRatioF();
    const QImage whole = window->screen()->grabWindow(window->winId()).toImage();
    const QPoint topLeft = widget->mapTo(window, QPoint(0, 0)) * scale;
    return whole.copy(QRect(topLeft, widget->size() * scale));
}

// XWayland does not let X11 clients read window contents back, so the screen cannot be checked
// there. It announces itself with an X extension of its own.
bool isXWayland() {
    const auto* x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    if (x11 == nullptr) {
        return false;
    }
    const char* name = "XWAYLAND";
    xcb_connection_t* connection = x11->connection();
    xcb_query_extension_reply_t* reply = xcb_query_extension_reply(
        connection, xcb_query_extension(connection, static_cast<uint16_t>(std::strlen(name)), name),
        nullptr);
    const bool present = reply != nullptr && reply->present != 0;
    std::free(reply);
    return present;
}

} // namespace

class ApplicationTest : public QObject {
    Q_OBJECT

public:
    static void initMain() { lcad::viewport::prepareApplication(); }

private slots:
    void init();

    void versionStringIsThreeNumbers();
    void mainWindowHostsTheViewport();
    void aboutTextNamesTheLibrariesInUse();
    void viewportAndMenusReachTheScreenTogether();
};

void ApplicationTest::init() {
    // Qt reports trouble with OpenGL contexts and surfaces as warnings and carries on.
    QTest::failOnWarning(QRegularExpression(QStringLiteral("OpenGL|QRhi|context|surface"),
                                            QRegularExpression::CaseInsensitiveOption));
}

void ApplicationTest::versionStringIsThreeNumbers() {
    const QRegularExpression pattern(QStringLiteral(R"(^\d+\.\d+\.\d+$)"));
    QVERIFY2(pattern.match(lcad::app::versionString()).hasMatch(),
             qPrintable(lcad::app::versionString()));
}

void ApplicationTest::mainWindowHostsTheViewport() {
    const MainWindow window;

    QCOMPARE(window.windowTitle(), QStringLiteral("LCAD"));
    QVERIFY(window.viewport() != nullptr);
    QCOMPARE(window.centralWidget(), window.viewport());
}

void ApplicationTest::aboutTextNamesTheLibrariesInUse() {
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    // Rendering a frame makes sure the viewport has an OpenGL context to describe.
    window.viewport()->grabFramebuffer();

    const QString about = window.aboutText();

    QVERIFY(about.contains(lcad::app::versionString()));
    QVERIFY(about.contains(QString::fromLatin1(qVersion())));
    QVERIFY(about.contains(QString::fromStdString(lcad::kernel::occtVersion())));
    QVERIFY(!window.viewport()->graphicsInfo().isEmpty());
    QVERIFY(about.contains(window.viewport()->graphicsInfo()));
}

// The viewport shares an OpenGL context with the code Qt uses to put every widget on screen.
// Getting that wrong leaves the widget's own framebuffer looking right while the window shows
// nothing, or shows menus with corrupted text. So this test looks at the screen.
void ApplicationTest::viewportAndMenusReachTheScreenTogether() {
    MainWindow window;
    window.resize(800, 600);
    window.viewport()->setShape(lcad::kernel::makeBox(100.0, 60.0, 40.0));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    if (isXWayland()) {
        QSKIP("XWayland does not let X11 clients read the screen; run this test with ctest.");
    }

    const QColor background = window.viewport()->backgroundColor();
    QTRY_VERIFY_WITH_TIMEOUT(drawnFraction(onScreen(window.viewport()), background) > 0.10, 5000);
    const QImage shownViewport = onScreen(window.viewport());
    const QImage shownMenuBar = onScreen(window.menuBar());
    keepFrame(window.screen()->grabWindow(window.winId()).toImage(), "main-window-on-screen");

    QVERIFY(drawnFraction(shownViewport, background) < 0.95);
    // The menu bar on screen is the menu bar Qt's own painter produces.
    const double menuBarDifference =
        differingFraction(shownMenuBar, window.menuBar()->grab().toImage());
    QVERIFY2(
        menuBarDifference < 0.01,
        qPrintable(QStringLiteral("menu bar differs in %1 of its pixels").arg(menuBarDifference)));
}

QTEST_MAIN(ApplicationTest)
#include "ApplicationTest.moc"
