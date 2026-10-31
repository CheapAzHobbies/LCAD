// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPoint>
#include <QScreen>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>

// Helpers for interface tests that have to answer something which blocks: a message box or a
// menu stops the code that opens it until it is closed, so the answer is arranged beforehand.
namespace lcad::test {

/// Whether this screen has room for a shown window to be `wanted` big. The screens of CI's
/// Windows and macOS machines are small, and those systems keep a window on the screen.
inline bool fitsOnScreen(const QWidget& window, QSize wanted) {
    const QSize frame = window.frameGeometry().size() - window.size();
    const QSize room = window.screen()->availableSize() - frame;
    return wanted.width() <= room.width() && wanted.height() <= room.height();
}

/// Moves the pointer over `widget`, there and then. QTest::mouseMove moves the real pointer,
/// and on macOS the event for that arrives when the system gets to it, which can be after the
/// press or the key a test sends next.
inline void movePointer(QWidget* widget, QPoint point) {
    QMouseEvent event(QEvent::MouseMove, point, widget->mapToGlobal(point), Qt::NoButton,
                      Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}

/// Presses `button` on the next message box that opens, and notes what the box said.
inline void answerNextMessageBox(QMessageBox::StandardButton button, QString* text = nullptr) {
    auto* timer = new QTimer(qApp);
    timer->setInterval(20);
    QObject::connect(timer, &QTimer::timeout, qApp, [timer, button, text] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (box == nullptr) {
            return;
        }
        // Stopped at once: a timer left running would answer the next box as well.
        timer->stop();
        timer->deleteLater();
        if (text != nullptr) {
            *text = box->text();
        }
        box->button(button)->click();
    });
    timer->start();
}

/// The entries of a menu as the user reads them: without access-key marks or separators.
inline QStringList entriesOf(const QMenu* menu) {
    QStringList entries;
    for (const QAction* entry : menu->actions()) {
        if (!entry->isSeparator()) {
            entries << entry->text().remove(QLatin1Char('&'));
        }
    }
    return entries;
}

/// Chooses the entry called `text` from the next menu that pops up, notes what the menu
/// offered, and closes it.
inline void chooseFromNextMenu(const QString& text, QStringList* offered = nullptr) {
    auto* timer = new QTimer(qApp);
    timer->setInterval(20);
    QObject::connect(timer, &QTimer::timeout, qApp, [timer, text, offered] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (menu == nullptr) {
            return;
        }
        timer->stop();
        timer->deleteLater();
        if (offered != nullptr) {
            *offered = entriesOf(menu);
        }
        for (QAction* entry : menu->actions()) {
            if (!entry->isSeparator() && entry->text().remove(QLatin1Char('&')) == text) {
                entry->trigger();
            }
        }
        menu->close();
    });
    timer->start();
}

} // namespace lcad::test
