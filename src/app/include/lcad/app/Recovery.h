// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/Document.h>

#include <QDateTime>
#include <QString>

#include <memory>
#include <vector>

class QLockFile;

namespace lcad::app {

/// Where LCAD keeps copies of unsaved work: recovery in its state folder (see stateFolder() in
/// Paths.h).
[[nodiscard]] QString defaultRecoveryFolder();

/// Copies of open documents with unsaved changes, kept apart from the user's files, so that
/// work survives a crash or a power cut.
///
/// Each running LCAD has a folder of its own in the recovery folder, locked for as long as it
/// runs. A folder whose lock is held by no running LCAD was left by one that did not close
/// properly: what is in it can be restored. Every copy is written whole under another name
/// and then put in place, as a save is, so a crash while writing leaves the previous copy.
class RecoveryStore {
public:
    explicit RecoveryStore(QString folder);
    ~RecoveryStore();

    RecoveryStore(const RecoveryStore&) = delete;
    RecoveryStore& operator=(const RecoveryStore&) = delete;

    /// Makes and locks this session's folder. Returns false if it cannot, and keeps no copies
    /// then.
    bool open();
    [[nodiscard]] bool isOpen() const { return m_lock != nullptr; }
    [[nodiscard]] QString folder() const { return m_folder; }
    [[nodiscard]] QString sessionFolder() const { return m_session; }

    /// Writes a copy of a document, under `key`, which tells the documents of this session
    /// apart. `originalPath` is the file it belongs to (empty if none), `title` what it is
    /// called. Returns false, and keeps the previous copy, if it cannot be written.
    bool save(const QString& key, const document::Document& document, const QString& originalPath,
              const QString& title);
    /// Forgets the copy of a document: it was saved, or closed without its changes.
    void discard(const QString& key);
    /// Forgets every copy of this session and its folder: LCAD is closing properly.
    void close();

    /// A copy left behind by an LCAD that did not close properly.
    struct Entry {
        QString file;
        QString originalPath;
        QString title;
        QDateTime saved;
        QString session;
    };
    /// What the folders of sessions that are no longer running hold, newest first.
    [[nodiscard]] std::vector<Entry> leftBehind() const;
    /// Removes the folder of a session that is no longer running.
    void removeSession(const QString& session) const;

private:
    QString m_folder;
    QString m_session;
    std::unique_ptr<QLockFile> m_lock;
};

} // namespace lcad::app
