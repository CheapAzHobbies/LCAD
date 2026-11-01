// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/LongWork.h>
#include <lcad/app/MainWindow.h>
#include <lcad/document/Document.h>
#include <lcad/kernel/Error.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Plane.h>
#include <lcad/kernel/Stop.h>
#include <lcad/sketch/Edit.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QPushButton>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <string>

using lcad::app::MainWindow;
using lcad::app::runLongWork;
using lcad::document::Document;
using lcad::document::Feature;
using lcad::document::FeatureKind;
using lcad::document::ObjectId;
namespace kernel = lcad::kernel;

namespace {

QWidget* box() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QStringLiteral("longWorkBox") && widget->isVisible()) {
            return widget;
        }
    }
    return nullptr;
}

// Waits for the box, and does something with it, while long work goes on.
void whenTheBoxShows(const std::function<void(QWidget*)>& act) {
    auto* polling = new QTimer(qApp);
    polling->setInterval(20);
    QObject::connect(polling, &QTimer::timeout, polling, [polling, act] {
        if (QWidget* shown = box()) {
            polling->stop();
            polling->deleteLater();
            act(shown);
        }
    });
    polling->start();
}

void showAndWait(QWidget& window) {
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
}

QPushButton* button(QWidget* in, const char* name) {
    return in->findChild<QPushButton*>(QLatin1String(name));
}

// Uses up the stack: about 4 KB a call.
std::size_t deep(std::size_t calls) {
    volatile char room[4096] = {};
    room[calls % sizeof(room)] = 1;
    const auto here = static_cast<std::size_t>(room[calls % sizeof(room)]);
    return calls == 0 ? here : deep(calls - 1) + here;
}

} // namespace

class LongWorkTest : public QObject {
    Q_OBJECT

private slots:
    void cleanup();

    void quickWorkShowsNothing();
    void whatWorkThrowsComesOut();
    void workWithinWorkIsDoneThere();
    void aSmallStackIsNotAProblem();
    void slowWorkCanBeStopped();
    void workCanBeLetRunForAsLongAsItTakes();
    void nothingElseTakesInputMeanwhile();
    void workThatWillNotStopIsSaidToBe();
    void aFaultInTheWorkDoesNotEndLcad();
    void aFeatureThatCrashesIsAFeatureThatFails();
    void aFeatureIsStoppedFromTheWindow();
};

void LongWorkTest::cleanup() {
    kernel::clearStop();
    lcad::app::setLongWorkPatience(8000);
    Document::setWorkRunner({});
}

void LongWorkTest::quickWorkShowsNothing() {
    bool done = false;
    runLongWork(QStringLiteral("Quick"), [&done] { done = true; });
    QVERIFY(done);
    QVERIFY(box() == nullptr);
}

void LongWorkTest::whatWorkThrowsComesOut() {
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, runLongWork(QStringLiteral("Failing"), [] {
                                 throw std::runtime_error("no good");
                             }));
    // Past the quick part too, and it can go again after.
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, runLongWork(QStringLiteral("Failing"), [] {
                                 QThread::msleep(300);
                                 throw std::runtime_error("no good");
                             }));
    bool done = false;
    runLongWork(QStringLiteral("Quick"), [&done] { done = true; });
    QVERIFY(done);
}

void LongWorkTest::workWithinWorkIsDoneThere() {
    QThread* outer = nullptr;
    QThread* inner = nullptr;
    runLongWork(QStringLiteral("Outer"), [&] {
        outer = QThread::currentThread();
        runLongWork(QStringLiteral("Inner"), [&] { inner = QThread::currentThread(); });
    });
    QVERIFY(outer != QThread::currentThread());
    QCOMPARE(inner, outer);
}

void LongWorkTest::aSmallStackIsNotAProblem() {
    // Some 80 MB of stack, which would end LCAD on any system's main or usual thread.
    std::size_t result = 1;
    runLongWork(QStringLiteral("Deep"), [&result] { result = deep(20000); });
    QVERIFY(result < 20002);
}

void LongWorkTest::slowWorkCanBeStopped() {
    MainWindow window;
    showAndWait(window);
    int ticks = 0;
    QTimer alive;
    connect(&alive, &QTimer::timeout, this, [&ticks] { ++ticks; });
    alive.start(10);
    QString shownName;
    whenTheBoxShows([&shownName](QWidget* shown) {
        shownName = shown->findChild<QLabel*>(QStringLiteral("longWorkName"))->text();
        QTest::mouseClick(button(shown, "longWorkStop"), Qt::LeftButton);
    });
    QElapsedTimer took;
    took.start();
    runLongWork(QStringLiteral("Fillet 3"), [] {
        while (!kernel::stopRequested()) {
            QThread::msleep(5);
        }
    });
    QVERIFY(kernel::stopRequested());
    QVERIFY(shownName.contains(QStringLiteral("Fillet 3")));
    // A moment before it showed, and the window went on all the while.
    QVERIFY(took.elapsed() >= 500);
    QVERIFY(ticks > 10);
    QVERIFY(box() == nullptr);
}

void LongWorkTest::workCanBeLetRunForAsLongAsItTakes() {
    bool shown = false;
    whenTheBoxShows([&shown](QWidget*) { shown = true; });
    bool done = false;
    runLongWork(QStringLiteral("Slow"), [&done] {
        QThread::msleep(1200);
        done = true;
    });
    QVERIFY(shown);
    QVERIFY(done);
    QVERIFY(!kernel::stopRequested());
    QTRY_VERIFY(box() == nullptr);
}

void LongWorkTest::nothingElseTakesInputMeanwhile() {
    QWidget window;
    auto* inside = new QPushButton(QStringLiteral("Do something"), &window);
    window.resize(200, 100);
    showAndWait(window);
    int clicks = 0;
    connect(inside, &QPushButton::clicked, this, [&clicks] { ++clicks; });
    // Past the time quick work is waited for, when other things could be going on.
    QTimer::singleShot(250, this, [inside] { QTest::mouseClick(inside, Qt::LeftButton); });
    // Nor does shutting the window take it away from under the work.
    QTimer::singleShot(300, this, [&window] { window.close(); });
    // And it is not drawn, which would get in the way of the work, until it is over.
    bool heldBack = false;
    QTimer::singleShot(350, this, [&window, &heldBack] { heldBack = !window.updatesEnabled(); });
    runLongWork(QStringLiteral("Slow"), [] { QThread::msleep(500); });
    QVERIFY(heldBack);
    QVERIFY(window.updatesEnabled());
    QCOMPARE(clicks, 0);
    QVERIFY(window.isVisible());
    QTest::mouseClick(inside, Qt::LeftButton);
    QCOMPARE(clicks, 1);
}

void LongWorkTest::workThatWillNotStopIsSaidToBe() {
    lcad::app::setLongWorkPatience(200);
    std::atomic<bool> release{false};
    bool offered = false;
    bool waitHid = false;
    whenTheBoxShows([&](QWidget* shown) {
        QTest::mouseClick(button(shown, "longWorkStop"), Qt::LeftButton);
        QVERIFY(!button(shown, "longWorkStop")->isEnabled());
        QTimer::singleShot(500, shown, [&, shown] {
            offered = button(shown, "longWorkWait")->isVisible() &&
                      button(shown, "longWorkClose")->isVisible();
            QTest::mouseClick(button(shown, "longWorkWait"), Qt::LeftButton);
            waitHid = !button(shown, "longWorkWait")->isVisible();
            release = true;
        });
    });
    runLongWork(QStringLiteral("Stubborn"), [&release] {
        while (!release) {
            QThread::msleep(5);
        }
    });
    QVERIFY(offered);
    QVERIFY(waitHid);
}

void LongWorkTest::aFaultInTheWorkDoesNotEndLcad() {
    int kept = 0;
    lcad::app::setLongWorkLastResort([&kept] { ++kept; });
    // A wild pointer, the stack running out, and the library giving up.
    const auto faulting = [&kept](const std::function<void()>& fault) {
        const int before = kept;
        QVERIFY_THROWS_EXCEPTION(kernel::Error, runLongWork(QStringLiteral("Fault"), fault));
        QCOMPARE(kept, before + 1);
    };
    faulting([] {
        volatile int* nowhere = nullptr;
        *nowhere = 1;
    });
    faulting([] { (void)deep(1000000000); });
#if !defined(Q_OS_WIN)
    // On Windows the runtime ends the program itself, with no fault to catch.
    faulting([] { std::abort(); });
#endif

    // All of which leaves things working.
    lcad::app::setLongWorkLastResort({});
    bool done = false;
    runLongWork(QStringLiteral("After"), [&done] { done = true; });
    QVERIFY(done);
}

void LongWorkTest::aFeatureThatCrashesIsAFeatureThatFails() {
    MainWindow window;
    showAndWait(window);
    Document* document = window.document();
    const ObjectId sketch = document->addSketch("Square", kernel::planeXY());
    lcad::sketch::Sketch square;
    (void)lcad::sketch::addRectangle(square, {0.0, 0.0}, {10.0, 10.0});
    document->setSketchGeometry(sketch, square, "Rectangle");
    Feature pull;
    pull.kind = FeatureKind::Pull;
    pull.sketch = sketch;
    pull.distance = 5.0;

    Document::setWorkRunner([](const std::string& name, const std::function<void()>&) {
        runLongWork(QString::fromStdString(name), [] {
            volatile int* nowhere = nullptr;
            *nowhere = 1;
        });
    });
    const ObjectId made = document->addFeature(pull);
    QVERIFY(
        QString::fromStdString(document->featureError(made)).contains(QStringLiteral("crashed")));

    // With the library well again, it builds.
    lcad::app::installLongWork();
    document->buildAgain(made);
    QVERIFY(document->featureError(made).empty());
}

void LongWorkTest::aFeatureIsStoppedFromTheWindow() {
    MainWindow window;
    showAndWait(window);
    Document* document = window.document();
    const ObjectId sketch = document->addSketch("Square", kernel::planeXY());
    lcad::sketch::Sketch square;
    (void)lcad::sketch::addRectangle(square, {0.0, 0.0}, {10.0, 10.0});
    document->setSketchGeometry(sketch, square, "Rectangle");
    Feature pull;
    pull.kind = FeatureKind::Pull;
    pull.sketch = sketch;
    pull.distance = 5.0;

    // As if it were a hard shape: real work, a long time coming.
    Document::setWorkRunner([](const std::string& name, const std::function<void()>& work) {
        runLongWork(QString::fromStdString(name), [&work] {
            QThread::msleep(1500);
            work();
        });
    });
    whenTheBoxShows(
        [](QWidget* shown) { QTest::mouseClick(button(shown, "longWorkStop"), Qt::LeftButton); });
    const ObjectId made = document->addFeature(pull);
    QVERIFY(
        QString::fromStdString(document->featureError(made)).contains(QStringLiteral("stopped")));
    QVERIFY(document->findBody(document->findFeature(made)->body)->shape.isNull());

    // Built again, with the time it needs.
    lcad::app::installLongWork();
    document->buildAgain(made);
    QVERIFY(document->featureError(made).empty());
    QCOMPARE(kernel::volume(document->findBody(document->findFeature(made)->body)->shape), 500.0);
}

QTEST_MAIN(LongWorkTest)
#include "LongWorkTest.moc"
