// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QFrame>

class QLineEdit;
class QListWidget;

namespace lcad::app {

class CommandRegistry;

/// The command search: a box that appears over the top of the window, lists the commands
/// matching what is typed, and starts the chosen one. With nothing typed it lists the commands
/// used most recently.
///
/// Enter starts the highlighted command, the arrow keys move the highlight, and Esc closes the
/// box. It closes when a command is started or the box loses the keyboard.
class CommandSearch : public QFrame {
    Q_OBJECT

public:
    CommandSearch(CommandRegistry* registry, QWidget* parent);

    /// Shows the box, empty, centered near the top of its parent.
    void open();
    [[nodiscard]] QLineEdit* field() const { return m_field; }
    /// The ids of the commands listed, top to bottom.
    [[nodiscard]] QStringList listedCommands() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void update(const QString& query);
    void runCurrent();

    CommandRegistry* m_registry;
    QLineEdit* m_field;
    QListWidget* m_list;
};

} // namespace lcad::app
