// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/CommandSearch.h>

#include <lcad/app/Commands.h>

#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

namespace lcad::app {

namespace {
constexpr int idRole = Qt::UserRole;
}

CommandSearch::CommandSearch(CommandRegistry* registry, QWidget* parent)
    : QFrame(parent), m_registry(registry), m_field(new QLineEdit(this)),
      m_list(new QListWidget(this)) {
    setObjectName(QStringLiteral("commandSearch"));
    setFrameShape(QFrame::StyledPanel);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);
    layout->addWidget(m_field);
    layout->addWidget(m_list);

    m_field->setObjectName(QStringLiteral("commandSearchField"));
    m_field->setPlaceholderText(tr("Search commands"));
    m_field->setClearButtonEnabled(true);
    m_field->installEventFilter(this);
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setUniformItemSizes(true);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    connect(m_field, &QLineEdit::textChanged, this, &CommandSearch::update);
    connect(m_list, &QListWidget::itemClicked, this, [this] { runCurrent(); });
    hide();
}

void CommandSearch::open() {
    const QWidget* window = parentWidget();
    const int width = qBound(320, window->width() / 3, 520);
    setFixedWidth(width);
    m_field->clear();
    update(QString());
    move((window->width() - width) / 2, 72);
    show();
    raise();
    m_field->setFocus();
}

QStringList CommandSearch::listedCommands() const {
    QStringList ids;
    for (int row = 0; row < m_list->count(); ++row) {
        ids << m_list->item(row)->data(idRole).toString();
    }
    return ids;
}

void CommandSearch::update(const QString& query) {
    m_registry->refresh();
    QStringList ids;
    if (query.trimmed().isEmpty()) {
        ids = m_registry->recent();
    } else {
        for (const CommandMatch& match : m_registry->search(query)) {
            ids << match.id;
        }
    }

    m_list->clear();
    for (const QString& id : std::as_const(ids)) {
        const Command* command = m_registry->command(id);
        const QAction* action = m_registry->action(id);
        if (command == nullptr || action == nullptr) {
            continue;
        }
        QString label = m_registry->displayName(id);
        if (!command->group.isEmpty()) {
            label = tr("%1  ·  %2").arg(label, command->group);
        }
        const QList<QKeySequence> shortcuts = action->shortcuts();
        if (!shortcuts.isEmpty()) {
            label += QStringLiteral("    ") + shortcuts.first().toString(QKeySequence::NativeText);
        }
        auto* item = new QListWidgetItem(action->icon(), label, m_list);
        item->setData(idRole, id);
        item->setToolTip(command->description);
        if (!action->isEnabled()) {
            // Listed, so that the user learns it exists, but not startable.
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        }
    }
    // The first command that can be started is the one Enter runs.
    for (int row = 0; row < m_list->count(); ++row) {
        if ((m_list->item(row)->flags() & Qt::ItemIsEnabled) != 0) {
            m_list->setCurrentRow(row);
            break;
        }
    }
    const int rows = qMin(m_list->count(), 10);
    m_list->setVisible(rows > 0);
    m_list->setFixedHeight(rows > 0 ? rows * m_list->sizeHintForRow(0) + 4 : 0);
    adjustSize();
}

void CommandSearch::runCurrent() {
    const QListWidgetItem* item = m_list->currentItem();
    if (item == nullptr || (item->flags() & Qt::ItemIsEnabled) == 0) {
        return;
    }
    const QString id = item->data(idRole).toString();
    hide();
    parentWidget()->setFocus();
    m_registry->trigger(id);
}

bool CommandSearch::eventFilter(QObject* watched, QEvent* event) {
    if (watched != m_field) {
        return QFrame::eventFilter(watched, event);
    }
    if (event->type() == QEvent::FocusOut && isVisible()) {
        hide();
        return false;
    }
    if (event->type() != QEvent::KeyPress) {
        return false;
    }
    const auto* key = static_cast<QKeyEvent*>(event);
    switch (key->key()) {
    case Qt::Key_Escape:
        hide();
        parentWidget()->setFocus();
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        runCurrent();
        return true;
    case Qt::Key_Down:
    case Qt::Key_Up: {
        // Step to the next command that can be started, in the direction of the arrow.
        const int step = key->key() == Qt::Key_Down ? 1 : -1;
        for (int row = m_list->currentRow() + step; row >= 0 && row < m_list->count();
             row += step) {
            if ((m_list->item(row)->flags() & Qt::ItemIsEnabled) != 0) {
                m_list->setCurrentRow(row);
                break;
            }
        }
        return true;
    }
    default:
        return false;
    }
}

} // namespace lcad::app
