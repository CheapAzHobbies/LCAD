// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/Commands.h>

#include <lcad/app/Icons.h>

#include <QActionGroup>
#include <QSettings>

#include <algorithm>

namespace lcad::app {

namespace {

const int recentLimit = 8;

QString settingsKey(const QString& id) {
    return QStringLiteral("shortcuts/") + id;
}

QString plain(QString text) {
    return text.remove(QLatin1Char('&')).remove(QStringLiteral("…"));
}

// What pointing at a button says: its name, and the key if it has one. What it does in more
// words goes to the status bar.
void nameInToolTip(QAction* action) {
    const QString name = plain(action->text());
    const QKeySequence key = action->shortcut();
    action->setToolTip(key.isEmpty() ? name
                                     : QStringLiteral("%1 (%2)").arg(
                                           name, key.toString(QKeySequence::NativeText)));
}

// How well `text` answers `query`: 0 for not at all.
int matchScore(const QString& text, const QString& query, int startOfText, int startOfWord,
               int elsewhere) {
    const qsizetype at = text.indexOf(query, 0, Qt::CaseInsensitive);
    if (at < 0) {
        return 0;
    }
    if (at == 0) {
        return startOfText;
    }
    // Look for an occurrence that begins a word.
    for (qsizetype i = at; i >= 0; i = text.indexOf(query, i + 1, Qt::CaseInsensitive)) {
        if (!text.at(i - 1).isLetterOrNumber()) {
            return startOfWord;
        }
    }
    return elsewhere;
}

} // namespace

CommandRegistry::CommandRegistry(QObject* parent) : QObject(parent) {}

QAction* CommandRegistry::add(Command command) {
    const QString id = command.id;
    auto* action = new QAction(command.text, this);
    action->setObjectName(id);
    action->setStatusTip(command.description);
    action->setCheckable(command.checkable);
    if (!command.iconName.isEmpty()) {
        action->setIcon(icon(command.iconName));
    }
    if (!command.exclusiveGroup.isEmpty()) {
        auto* group = findChild<QActionGroup*>(command.exclusiveGroup);
        if (group == nullptr) {
            group = new QActionGroup(this);
            group->setObjectName(command.exclusiveGroup);
        }
        group->addAction(action);
    }

    m_defaults.insert(id, command.shortcuts);
    const QVariant stored = QSettings().value(settingsKey(id));
    if (stored.isValid()) {
        QList<QKeySequence> sequences;
        for (const QString& text : stored.toStringList()) {
            if (!text.isEmpty()) {
                sequences << QKeySequence(text, QKeySequence::PortableText);
            }
        }
        action->setShortcuts(sequences);
    } else {
        action->setShortcuts(command.shortcuts);
    }
    nameInToolTip(action);

    connect(action, &QAction::triggered, this, [this, id](bool checked) {
        const Command& definition = m_commands[id];
        if (definition.run) {
            definition.run(checked);
        }
        if (definition.remembered) {
            noteUsed(id);
        }
        emit triggered(id);
        refresh();
    });

    m_commands.insert(id, std::move(command));
    m_actions.insert(id, action);
    m_order << id;
    return action;
}

QAction* CommandRegistry::action(const QString& id) const {
    return m_actions.value(id, nullptr);
}

const Command* CommandRegistry::command(const QString& id) const {
    const auto found = m_commands.constFind(id);
    return found == m_commands.constEnd() ? nullptr : &found.value();
}

QString CommandRegistry::displayName(const QString& id) const {
    const QAction* found = action(id);
    return found != nullptr ? plain(found->text()) : QString();
}

void CommandRegistry::refresh() {
    for (const QString& id : std::as_const(m_order)) {
        const Command& definition = m_commands[id];
        QAction* action = m_actions[id];
        const bool available = !m_available || m_available(id);
        action->setEnabled(available && (!definition.isEnabled || definition.isEnabled()));
        if (definition.checkable && definition.isChecked) {
            action->setChecked(definition.isChecked());
        }
        if (definition.currentText) {
            action->setText(definition.currentText());
            nameInToolTip(action);
        }
    }
}

bool CommandRegistry::trigger(const QString& id) {
    QAction* found = action(id);
    if (found == nullptr) {
        return false;
    }
    refresh();
    if (!found->isEnabled()) {
        return false;
    }
    found->trigger();
    return true;
}

void CommandRegistry::noteUsed(const QString& id) {
    m_recent.removeAll(id);
    m_recent.prepend(id);
    while (m_recent.size() > recentLimit) {
        m_recent.removeLast();
    }
}

QList<CommandMatch> CommandRegistry::search(const QString& query, int limit) const {
    const QString wanted = query.trimmed();
    QList<CommandMatch> matches;
    if (wanted.isEmpty()) {
        return matches;
    }
    for (const QString& id : m_order) {
        const Command& definition = m_commands[id];
        int score = matchScore(displayName(id), wanted, 100, 80, 60);
        for (const QString& alias : definition.aliases) {
            score = std::max(score, matchScore(alias, wanted, 55, 45, 35));
        }
        score = std::max(score, matchScore(definition.group, wanted, 25, 20, 15));
        score = std::max(score, matchScore(definition.description, wanted, 12, 10, 8));
        if (score == 0) {
            continue;
        }
        // What can be done now is what the user most likely means.
        if (!m_actions[id]->isEnabled()) {
            score -= 1000;
        }
        matches.append({id, score});
    }
    std::stable_sort(
        matches.begin(), matches.end(),
        [](const CommandMatch& a, const CommandMatch& b) { return a.score > b.score; });
    if (matches.size() > limit) {
        matches.resize(limit);
    }
    return matches;
}

QList<QKeySequence> CommandRegistry::shortcuts(const QString& id) const {
    const QAction* found = action(id);
    return found != nullptr ? found->shortcuts() : QList<QKeySequence>();
}

QList<QKeySequence> CommandRegistry::defaultShortcuts(const QString& id) const {
    return m_defaults.value(id);
}

QString CommandRegistry::commandWithShortcut(const QKeySequence& sequence,
                                             const QString& except) const {
    if (sequence.isEmpty()) {
        return {};
    }
    for (const QString& id : m_order) {
        if (id != except && m_actions[id]->shortcuts().contains(sequence)) {
            return id;
        }
    }
    return {};
}

bool CommandRegistry::setShortcuts(const QString& id, const QList<QKeySequence>& sequences) {
    QAction* found = action(id);
    if (found == nullptr) {
        return false;
    }
    for (const QKeySequence& sequence : sequences) {
        if (!commandWithShortcut(sequence, id).isEmpty()) {
            return false;
        }
    }
    found->setShortcuts(sequences);
    nameInToolTip(found);
    if (sequences == m_defaults.value(id)) {
        QSettings().remove(settingsKey(id));
    } else {
        QStringList texts;
        for (const QKeySequence& sequence : sequences) {
            texts << sequence.toString(QKeySequence::PortableText);
        }
        // An empty list would read back as "never set"; an empty entry says "none".
        QSettings().setValue(settingsKey(id), texts.isEmpty() ? QStringList{QString()} : texts);
    }
    return true;
}

void CommandRegistry::resetShortcuts() {
    QSettings settings;
    settings.remove(QStringLiteral("shortcuts"));
    for (const QString& id : std::as_const(m_order)) {
        m_actions[id]->setShortcuts(m_defaults.value(id));
    }
}

} // namespace lcad::app
