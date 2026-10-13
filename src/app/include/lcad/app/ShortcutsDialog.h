// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

class QKeySequenceEdit;
class QLabel;
class QTableWidget;

namespace lcad::app {

class CommandRegistry;

/// The editor for keyboard shortcuts: every command with its shortcut, which can be replaced
/// by pressing the new keys. A shortcut that already belongs to another command is refused,
/// with a message saying which. Changes apply at once and are remembered.
class ShortcutsDialog : public QDialog {
    Q_OBJECT

public:
    ShortcutsDialog(CommandRegistry* registry, QWidget* parent);

    /// The key box for a command, or nullptr.
    [[nodiscard]] QKeySequenceEdit* editorFor(const QString& commandId) const;
    /// What the dialog last said about a change: empty when it was accepted.
    [[nodiscard]] QString message() const;

private:
    void fill();
    void apply(const QString& id, QKeySequenceEdit* editor);

    CommandRegistry* m_registry;
    QTableWidget* m_table;
    QLabel* m_message;
};

} // namespace lcad::app
