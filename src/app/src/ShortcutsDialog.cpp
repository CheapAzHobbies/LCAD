// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/ShortcutsDialog.h>

#include <lcad/app/Commands.h>

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace lcad::app {

ShortcutsDialog::ShortcutsDialog(CommandRegistry* registry, QWidget* parent)
    : QDialog(parent), m_registry(registry), m_table(new QTableWidget(this)),
      m_message(new QLabel(this)) {
    setWindowTitle(tr("Keyboard Shortcuts"));
    resize(560, 520);
    auto* layout = new QVBoxLayout(this);
    auto* hint = new QLabel(
        tr("Click a shortcut and press the new keys. Press Backspace in the box to clear it."),
        this);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addWidget(m_table);
    m_message->setObjectName(QStringLiteral("errorText"));
    m_message->setWordWrap(true);
    layout->addWidget(m_message);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QPushButton* reset = buttons->addButton(tr("Restore Defaults"), QDialogButtonBox::ResetRole);
    reset->setObjectName(QStringLiteral("restoreDefaults"));
    connect(reset, &QPushButton::clicked, this, [this] {
        m_registry->resetShortcuts();
        m_message->clear();
        fill();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    m_table->setColumnCount(3);
    m_table->setHorizontalHeaderLabels({tr("Command"), tr("Group"), tr("Shortcut")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_table->setColumnWidth(2, 170);
    m_table->verticalHeader()->hide();
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    fill();
}

QKeySequenceEdit* ShortcutsDialog::editorFor(const QString& commandId) const {
    return m_table->findChild<QKeySequenceEdit*>(commandId);
}

QString ShortcutsDialog::message() const {
    return m_message->text();
}

void ShortcutsDialog::fill() {
    const QStringList ids = m_registry->ids();
    if (m_table->rowCount() == ids.size()) {
        // Already listed: show the shortcuts as they are now.
        for (const QString& id : ids) {
            const QList<QKeySequence> shortcuts = m_registry->shortcuts(id);
            editorFor(id)->setKeySequence(shortcuts.isEmpty() ? QKeySequence() : shortcuts.first());
        }
        return;
    }
    m_table->setRowCount(static_cast<int>(ids.size()));
    for (int row = 0; row < ids.size(); ++row) {
        const QString& id = ids.at(row);
        const Command* command = m_registry->command(id);
        auto* name = new QTableWidgetItem(command->text.isEmpty()
                                              ? m_registry->displayName(id)
                                              : QString(command->text).remove(QLatin1Char('&')));
        name->setToolTip(command->description);
        m_table->setItem(row, 0, name);
        m_table->setItem(row, 1, new QTableWidgetItem(command->group));
        const QList<QKeySequence> shortcuts = m_registry->shortcuts(id);
        auto* editor =
            new QKeySequenceEdit(shortcuts.isEmpty() ? QKeySequence() : shortcuts.first(), m_table);
        editor->setObjectName(id);
        connect(editor, &QKeySequenceEdit::editingFinished, this,
                [this, id, editor] { apply(id, editor); });
        m_table->setCellWidget(row, 2, editor);
    }
}

void ShortcutsDialog::apply(const QString& id, QKeySequenceEdit* editor) {
    const QKeySequence wanted = editor->keySequence();
    const QList<QKeySequence> current = m_registry->shortcuts(id);
    // A command's further shortcuts stay; the box edits the first.
    QList<QKeySequence> sequences = current.mid(1);
    if (!wanted.isEmpty()) {
        sequences.prepend(wanted);
    }
    if (m_registry->setShortcuts(id, sequences)) {
        m_message->clear();
        return;
    }
    const QString owner = m_registry->commandWithShortcut(wanted, id);
    m_message->setText(
        tr("%1 already belongs to \"%2\". Change that one first.")
            .arg(wanted.toString(QKeySequence::NativeText), m_registry->displayName(owner)));
    editor->setKeySequence(current.isEmpty() ? QKeySequence() : current.first());
}

} // namespace lcad::app
