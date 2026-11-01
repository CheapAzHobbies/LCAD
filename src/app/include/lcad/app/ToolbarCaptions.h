// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QAction>
#include <QEvent>
#include <QList>
#include <QPainter>
#include <QPaintEvent>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QToolBar>
#include <QWidget>

#include <utility>

namespace lcad::app {

// The names of a toolbar's groups, in small letters under the tools. A separator ends a group,
// and the tools are laid out as they would be without this: it only needs a strip of the
// toolbar kept free for it. Every toolbar has one: they take each other's place, and if they
// were not all of one height the 3D view would change size with the workspace.
class ToolbarCaptions : public QWidget {
public:
    static constexpr int stripHeight = 13;

    /// `names` has one for each group, in order. `end` is the first thing on the toolbar that
    /// belongs to no group, if there is one.
    ToolbarCaptions(QToolBar* toolbar, QStringList names, QWidget* end = nullptr)
        : QWidget(toolbar), m_toolbar(toolbar), m_names(std::move(names)), m_end(end) {
        setObjectName(QStringLiteral("toolbarCaptions"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        toolbar->setContentsMargins(0, 0, 0, stripHeight);
        toolbar->installEventFilter(this);
        place();
    }

    /// The groups' names and where they are drawn, for the tests.
    [[nodiscard]] QList<std::pair<QString, QRect>> captions() const {
        QList<std::pair<QString, QRect>> placed;
        qsizetype group = 0;
        QRect span;
        const auto finish = [&] {
            if (group < m_names.size() && span.isValid()) {
                placed.append({m_names[group], QRect(span.left(), 0, span.width(), height())});
            }
            span = QRect();
            ++group;
        };
        for (QAction* entry : m_toolbar->actions()) {
            if (entry->isSeparator()) {
                finish();
                continue;
            }
            const QWidget* widget = m_toolbar->widgetForAction(entry);
            if (widget == m_end && m_end != nullptr) {
                break;
            }
            if (widget != nullptr && !widget->isHidden() &&
                widget->sizePolicy().horizontalPolicy() != QSizePolicy::Expanding) {
                span = span.united(widget->geometry());
            }
        }
        finish();
        return placed;
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == m_toolbar &&
            (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest ||
             event->type() == QEvent::Show)) {
            place();
        }
        return QWidget::eventFilter(watched, event);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        QFont small = font();
        small.setPointSizeF(small.pointSizeF() * 0.72);
        small.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
        painter.setFont(small);
        painter.setPen(palette().color(QPalette::Disabled, QPalette::WindowText));
        for (const auto& [name, where] : captions()) {
            painter.drawText(where, Qt::AlignCenter | Qt::TextDontClip, name.toUpper());
        }
    }

private:
    void place() {
        setGeometry(0, m_toolbar->height() - stripHeight - 1, m_toolbar->width(), stripHeight);
        raise();
        update();
    }

    QToolBar* m_toolbar;
    QStringList m_names;
    QWidget* m_end;
};

} // namespace lcad::app
