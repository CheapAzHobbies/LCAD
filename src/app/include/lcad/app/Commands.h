// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QAction>
#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

namespace lcad::app {

/// Everything LCAD knows about one command. A command is defined once, here, and the toolbar,
/// the menus, context menus, keyboard shortcuts, and the command search all start it through
/// the same definition.
struct Command {
    /// Stable identifier, for example "create.box". Settings and tests refer to commands by it.
    QString id;
    /// The name as menus show it, with "&" before the access key: "&Box".
    QString text;
    /// One sentence saying what it does, for tooltips, the status bar, and search results.
    QString description;
    /// The group it belongs to in the toolbar and in search results: "Create", "View".
    QString group;
    /// Other words a user might look for it by: "cube", "block".
    QStringList aliases;
    QList<QKeySequence> shortcuts;
    /// The name of its icon (see Icons.h). May be empty.
    QString iconName;
    /// A command that is on or off, such as "Perspective".
    bool checkable = false;
    /// Of the checkable commands sharing this name, exactly one is on at a time.
    QString exclusiveGroup;
    /// Whether starting it puts it on the list of recent commands. The search itself does not
    /// belong there.
    bool remembered = true;

    /// What the command does. For a checkable command the argument is its new state.
    std::function<void(bool)> run;
    /// Whether it can be started now. Unset means always.
    std::function<bool()> isEnabled;
    /// Whether a checkable command is on now.
    std::function<bool()> isChecked;
    /// A name that depends on the state, such as "Undo Create Box". Unset means `text`.
    std::function<QString()> currentText;
};

/// A command found by a search, best match first.
struct CommandMatch {
    QString id;
    int score = 0;
};

/// The commands of one window.
class CommandRegistry : public QObject {
    Q_OBJECT

public:
    explicit CommandRegistry(QObject* parent = nullptr);

    /// Defines a command and returns the action that stands for it everywhere. Shortcuts the
    /// user chose earlier replace the command's own.
    QAction* add(Command command);

    [[nodiscard]] QAction* action(const QString& id) const;
    [[nodiscard]] const Command* command(const QString& id) const;
    /// All command ids, in the order they were defined.
    [[nodiscard]] QStringList ids() const { return m_order; }
    /// The name without the access-key mark, in its current form: "Undo Create Box".
    [[nodiscard]] QString displayName(const QString& id) const;

    /// Brings every action in line with the state of the application: enabled, checked, name,
    /// and icon. Call after anything the commands depend on has changed.
    void refresh();

    /// Decides, over any command's own say, which commands can be started at all now: a
    /// workspace has its own. Unset means all.
    void setAvailability(std::function<bool(const QString& id)> available) {
        m_available = std::move(available);
    }

    /// Starts a command as a click on its button would. Returns false, and does nothing, when
    /// there is no such command or it cannot be started now.
    bool trigger(const QString& id);

    // Search

    /// The commands matching what was typed, best first. Matches the start of the name best,
    /// then the start of any word in it, then other parts of it, then aliases, then the group
    /// and description. Commands that cannot be started now come after those that can.
    [[nodiscard]] QList<CommandMatch> search(const QString& query, int limit = 12) const;
    /// The commands started most recently, newest first.
    [[nodiscard]] QStringList recent() const { return m_recent; }

    // Shortcuts

    [[nodiscard]] QList<QKeySequence> shortcuts(const QString& id) const;
    [[nodiscard]] QList<QKeySequence> defaultShortcuts(const QString& id) const;
    /// The command a key sequence already belongs to, or an empty string.
    [[nodiscard]] QString commandWithShortcut(const QKeySequence& sequence,
                                              const QString& except = {}) const;
    /// Gives a command other shortcuts and remembers them. Returns false, changing nothing,
    /// if one of them belongs to another command.
    bool setShortcuts(const QString& id, const QList<QKeySequence>& sequences);
    /// Gives every command its own shortcuts back.
    void resetShortcuts();

signals:
    /// A command was started, by whatever means.
    void triggered(const QString& id);

private:
    std::function<bool(const QString& id)> m_available;
    void noteUsed(const QString& id);

    QHash<QString, Command> m_commands;
    QHash<QString, QAction*> m_actions;
    QHash<QString, QList<QKeySequence>> m_defaults;
    QStringList m_order;
    QStringList m_recent;
};

} // namespace lcad::app
