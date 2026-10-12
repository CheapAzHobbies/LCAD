#include <lcad/app/MainWindow.h>

#include <lcad/app/Application.h>
#include <lcad/kernel/Version.h>
#include <lcad/viewport/ViewportWidget.h>

#include <QAction>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>

namespace lcad::app {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), m_viewport(new viewport::ViewportWidget(this)) {
    setWindowTitle(QStringLiteral("LCAD"));
    resize(1280, 800);
    setCentralWidget(m_viewport);

    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    QAction* quitAction = fileMenu->addAction(tr("&Quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
    QAction* aboutAction = helpMenu->addAction(tr("&About LCAD"));
    connect(aboutAction, &QAction::triggered, this, &MainWindow::showAbout);

    // Queued: the viewport reports failure while painting, which is no place to open a dialog.
    connect(m_viewport, &viewport::ViewportWidget::initializationFailed, this,
            &MainWindow::showViewportError, Qt::QueuedConnection);
}

QString MainWindow::aboutText() const {
    QString text = tr("LCAD %1\n\nBuilt with Qt %2 and Open CASCADE Technology %3.")
                       .arg(versionString(), QString::fromLatin1(qVersion()),
                            QString::fromStdString(kernel::occtVersion()));
    const QString graphics = m_viewport->graphicsInfo();
    if (!graphics.isEmpty()) {
        text += QStringLiteral("\n\n") + graphics;
    }
    return text;
}

void MainWindow::showAbout() {
    QMessageBox::about(this, tr("About LCAD"), aboutText());
}

void MainWindow::showViewportError(const QString& reason) {
    QMessageBox::critical(this, tr("3D viewport unavailable"), reason);
}

} // namespace lcad::app
