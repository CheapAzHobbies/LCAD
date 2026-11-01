// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/LongWork.h>

#include <lcad/document/Document.h>
#include <lcad/kernel/Stop.h>

#include <QApplication>
#include <QCloseEvent>
#include <QDeadlineTimer>
#include <QDialog>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QList>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <utility>

namespace lcad::app {

namespace {

// Work this quick is simply waited for: nothing can come in between, and nothing is shown.
constexpr int quickWork = 150;
// How long before the box appears.
constexpr int shownAfter = 700;
// How long work has, once it is asked to stop, before the box says that it has not.
int stopsWithin = 8000;
// Virtual memory, of which only what is used is ever taken. A default thread has 512 KB on macOS
// and 1 MB on Windows, and geometry that recurses deeply runs through that and takes LCAD with it.
constexpr std::size_t workStack = std::size_t{256} * 1024 * 1024;

bool running = false;

std::function<void()>& lastResort() {
    static std::function<void()> keep;
    return keep;
}

class WorkThread final : public QThread {
public:
    explicit WorkThread(const std::function<void()>& work) : m_work(work) {
        setStackSize(static_cast<uint>(workStack));
    }

    [[nodiscard]] std::exception_ptr failure() const { return m_failure; }

protected:
    void run() override {
        try {
            m_work();
        } catch (...) {
            m_failure = std::current_exception();
        }
    }

private:
    const std::function<void()>& m_work;
    std::exception_ptr m_failure;
};

class WorkBox final : public QDialog {
public:
    explicit WorkBox(const QString& name)
        : QDialog(QApplication::activeWindow()), m_time(new QLabel(this)), m_note(new QLabel(this)),
          m_stop(new QPushButton(QObject::tr("Stop"), this)),
          m_wait(new QPushButton(QObject::tr("Keep Waiting"), this)),
          m_close(new QPushButton(QObject::tr("Save and Close LCAD"), this)) {
        setObjectName(QStringLiteral("longWorkBox"));
        setWindowTitle(QObject::tr("Working"));
        setWindowModality(Qt::ApplicationModal);
        setMinimumWidth(340);
        auto* layout = new QVBoxLayout(this);
        auto* what = new QLabel(QObject::tr("Working on %1").arg(name), this);
        what->setObjectName(QStringLiteral("longWorkName"));
        what->setWordWrap(true);
        layout->addWidget(what);
        auto* bar = new QProgressBar(this);
        bar->setRange(0, 0);
        bar->setTextVisible(false);
        layout->addWidget(bar);
        m_time->setObjectName(QStringLiteral("longWorkTime"));
        layout->addWidget(m_time);
        m_note->setObjectName(QStringLiteral("longWorkNote"));
        m_note->setWordWrap(true);
        m_note->setText(QObject::tr("This can go on for as long as you like. Stop leaves the "
                                    "model as it was before this step."));
        layout->addWidget(m_note);
        auto* buttons = new QHBoxLayout;
        buttons->addStretch(1);
        m_stop->setObjectName(QStringLiteral("longWorkStop"));
        m_wait->setObjectName(QStringLiteral("longWorkWait"));
        m_close->setObjectName(QStringLiteral("longWorkClose"));
        buttons->addWidget(m_close);
        buttons->addWidget(m_wait);
        buttons->addWidget(m_stop);
        layout->addLayout(buttons);
        m_wait->hide();
        m_close->hide();

        QObject::connect(m_stop, &QPushButton::clicked, this, [this] { stop(); });
        QObject::connect(m_wait, &QPushButton::clicked, this, [this] {
            m_wait->hide();
            m_close->hide();
            m_note->setText(QObject::tr("Still waiting for it to stop."));
        });
        QObject::connect(m_close, &QPushButton::clicked, this, [] {
            if (lastResort()) {
                lastResort()();
            }
            // Not a normal exit: the work is still going on, on data that would go away under it.
            std::_Exit(EXIT_FAILURE);
        });
        m_stopping.setSingleShot(true);
        m_stopping.setInterval(stopsWithin);
        QObject::connect(&m_stopping, &QTimer::timeout, this, [this] { notStopping(); });
        m_ticking.setInterval(250);
        QObject::connect(&m_ticking, &QTimer::timeout, this, [this] { showTime(); });
        m_elapsed.start();
        showTime();
        m_ticking.start();
    }

protected:
    // Shutting the box, or Escape, is the same as Stop: never a way to leave the work running.
    void closeEvent(QCloseEvent* event) override {
        event->ignore();
        stop();
    }
    void reject() override { stop(); }

private:
    void stop() {
        if (m_stop->isEnabled()) {
            m_stop->setEnabled(false);
            m_stop->setText(QObject::tr("Stopping…"));
            m_note->setText(QObject::tr("Stopping as soon as it can."));
            kernel::requestStop();
            m_stopping.start();
        }
    }
    void notStopping() {
        m_note->setText(QObject::tr("It has not stopped: the geometry library is deep in something "
                                    "it does not look up from. You can wait for it, or close LCAD, "
                                    "which saves your work for you to recover without this step."));
        m_wait->show();
        m_close->show();
    }
    void showTime() {
        const qint64 seconds = m_elapsed.elapsed() / 1000;
        m_time->setText(QObject::tr("%1:%2 so far")
                            .arg(seconds / 60)
                            .arg(seconds % 60, 2, 10, QLatin1Char('0')));
    }

    QLabel* m_time;
    QLabel* m_note;
    QPushButton* m_stop;
    QPushButton* m_wait;
    QPushButton* m_close;
    QTimer m_stopping;
    QTimer m_ticking;
    QElapsedTimer m_elapsed;
};

// Input is not for anything but the box while work goes on: the document is being read.
class InputBlocker final : public QObject {
public:
    explicit InputBlocker(const QWidget* box) : m_box(box) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:
        case QEvent::Wheel:
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
        case QEvent::Shortcut:
        case QEvent::ShortcutOverride:
        case QEvent::ContextMenu:
        case QEvent::Close:
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
        case QEvent::DragEnter:
        case QEvent::DragMove:
        case QEvent::Drop:
        case QEvent::FileOpen:
            return !isBox(watched);
        default:
            return false;
        }
    }

private:
    [[nodiscard]] bool isBox(const QObject* watched) const {
        if (watched == m_box->windowHandle()) {
            return true;
        }
        const auto* widget = qobject_cast<const QWidget*>(watched);
        return widget != nullptr && (widget == m_box || m_box->isAncestorOf(widget));
    }

    const QWidget* m_box;
};

// The windows are not drawn while work goes on, for as long as this lasts. Drawing the 3D view
// picks up where whatever asked for the work left off, which is still in the middle of something,
// and it makes meshes for the shapes the work is reading, which OCCT does not allow.
class WindowsHeld {
public:
    WindowsHeld() {
        for (QWidget* window : QApplication::topLevelWidgets()) {
            if (window->updatesEnabled()) {
                window->setUpdatesEnabled(false);
                m_windows.append(window);
            }
        }
    }
    ~WindowsHeld() {
        for (const QPointer<QWidget>& window : m_windows) {
            if (window != nullptr) {
                window->setUpdatesEnabled(true);
            }
        }
    }
    WindowsHeld(const WindowsHeld&) = delete;
    WindowsHeld& operator=(const WindowsHeld&) = delete;

private:
    QList<QPointer<QWidget>> m_windows;
};

} // namespace

void runLongWork(const QString& name, const std::function<void()>& work) {
    if (running || qApp == nullptr || QThread::currentThread() != qApp->thread()) {
        work();
        return;
    }
    running = true;
    WorkThread thread(work);
    QEventLoop loop;
    QObject::connect(&thread, &QThread::finished, &loop, &QEventLoop::quit);
    thread.start();
    if (!thread.wait(QDeadlineTimer(quickWork))) {
        const WindowsHeld held;
        WorkBox box(name);
        InputBlocker blocker(&box);
        qApp->installEventFilter(&blocker);
        QTimer showing;
        showing.setSingleShot(true);
        QObject::connect(&showing, &QTimer::timeout, &box, [&box] { box.show(); });
        showing.start(shownAfter);
        loop.exec();
        thread.wait();
        qApp->removeEventFilter(&blocker);
    }
    running = false;
    if (thread.failure()) {
        std::rethrow_exception(thread.failure());
    }
}

void installLongWork() {
    document::Document::setWorkRunner(
        [](const std::string& name, const std::function<void()>& work) {
            runLongWork(QString::fromStdString(name), work);
        });
}

void setLongWorkLastResort(std::function<void()> keepWhatCanBeKept) {
    lastResort() = std::move(keepWhatCanBeKept);
}

void setLongWorkPatience(int milliseconds) {
    stopsWithin = milliseconds;
}

} // namespace lcad::app
