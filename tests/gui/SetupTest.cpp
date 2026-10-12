#include <lcad/viewport/ViewportWidget.h>

#include <QCoreApplication>
#include <QSurfaceFormat>
#include <QTest>

// prepareApplication() has to run before QApplication exists, so these tests run without one.
class SetupTest : public QObject {
    Q_OBJECT

private slots:
    void platformDefaultsToX11();
    void explicitPlatformChoiceIsKept();
    void defaultSurfaceFormatIsTheViewports();
    void contextsShareResources();
};

void SetupTest::platformDefaultsToX11() {
    qunsetenv("QT_QPA_PLATFORM");

    lcad::viewport::prepareApplication();

#if defined(Q_OS_LINUX)
    QCOMPARE(qgetenv("QT_QPA_PLATFORM"), QByteArray("xcb"));
#else
    QVERIFY(!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"));
#endif
}

void SetupTest::explicitPlatformChoiceIsKept() {
    qputenv("QT_QPA_PLATFORM", "offscreen");

    lcad::viewport::prepareApplication();

    QCOMPARE(qgetenv("QT_QPA_PLATFORM"), QByteArray("offscreen"));
}

void SetupTest::defaultSurfaceFormatIsTheViewports() {
    lcad::viewport::prepareApplication();

    const QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    QCOMPARE(format, lcad::viewport::ViewportWidget::surfaceFormat());
    // OCCT's viewer needs both of these.
    QVERIFY(format.depthBufferSize() >= 24);
    QVERIFY(format.stencilBufferSize() >= 8);
}

void SetupTest::contextsShareResources() {
    lcad::viewport::prepareApplication();

    QVERIFY(QCoreApplication::testAttribute(Qt::AA_ShareOpenGLContexts));
}

QTEST_APPLESS_MAIN(SetupTest)
#include "SetupTest.moc"
