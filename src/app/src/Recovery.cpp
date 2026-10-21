// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/Recovery.h>

#include <lcad/core/Log.h>
#include <lcad/document/File.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>
#include <filesystem>

namespace lcad::app {

namespace {

const char* const lockName = "lock";

std::filesystem::path toPath(const QString& path) {
    return std::filesystem::path(path.toStdString());
}

} // namespace

QString defaultRecoveryFolder() {
    QString base = qEnvironmentVariable("XDG_STATE_HOME");
    if (base.isEmpty()) {
        base = QDir::homePath() + QStringLiteral("/.local/state");
    }
    return base + QStringLiteral("/lcad/recovery");
}

RecoveryStore::RecoveryStore(QString folder) : m_folder(std::move(folder)) {}

RecoveryStore::~RecoveryStore() = default;

bool RecoveryStore::open() {
    if (m_lock != nullptr) {
        return true;
    }
    const QString session = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString path = m_folder + QLatin1Char('/') + session;
    if (!QDir().mkpath(path)) {
        core::log(core::LogLevel::Warning, "recovery",
                  "cannot make " + path.toStdString() + "; unsaved work is not kept");
        return false;
    }
    auto lock = std::make_unique<QLockFile>(path + QLatin1Char('/') + QLatin1String(lockName));
    // A lock is taken over only from an LCAD that is no longer running, never because it is
    // old.
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0)) {
        return false;
    }
    m_session = path;
    m_lock = std::move(lock);
    return true;
}

bool RecoveryStore::save(const QString& key, const document::Document& document,
                         const QString& originalPath, const QString& title) {
    if (m_lock == nullptr) {
        return false;
    }
    const QString file = m_session + QLatin1Char('/') + key + QStringLiteral(".lcad");
    try {
        document::saveDocument(document, toPath(file));
    } catch (const document::FileError& error) {
        core::log(core::LogLevel::Warning, "recovery", error.what());
        return false;
    }
    // What the copy is of, beside it.
    QSaveFile note(m_session + QLatin1Char('/') + key + QStringLiteral(".json"));
    if (!note.open(QIODevice::WriteOnly)) {
        return false;
    }
    const QJsonObject about{
        {QStringLiteral("originalPath"), originalPath},
        {QStringLiteral("title"), title},
        {QStringLiteral("saved"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    note.write(QJsonDocument(about).toJson());
    return note.commit();
}

void RecoveryStore::discard(const QString& key) {
    if (m_lock == nullptr) {
        return;
    }
    QFile::remove(m_session + QLatin1Char('/') + key + QStringLiteral(".lcad"));
    QFile::remove(m_session + QLatin1Char('/') + key + QStringLiteral(".json"));
}

void RecoveryStore::close() {
    if (m_lock == nullptr) {
        return;
    }
    m_lock->unlock();
    m_lock.reset();
    QDir(m_session).removeRecursively();
    m_session.clear();
}

std::vector<RecoveryStore::Entry> RecoveryStore::leftBehind() const {
    std::vector<Entry> entries;
    const QDir root(m_folder);
    for (const QFileInfo& session : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (session.absoluteFilePath() == QFileInfo(m_session).absoluteFilePath()) {
            continue;
        }
        // Held by an LCAD that is still running: not left behind.
        QLockFile lock(session.absoluteFilePath() + QLatin1Char('/') + QLatin1String(lockName));
        lock.setStaleLockTime(0);
        if (!lock.tryLock(0)) {
            continue;
        }
        lock.unlock();
        const QDir folder(session.absoluteFilePath());
        const QFileInfoList copies = folder.entryInfoList({QStringLiteral("*.lcad")}, QDir::Files);
        if (copies.isEmpty()) {
            // An LCAD that ended without closing properly, but with nothing unsaved.
            QDir(session.absoluteFilePath()).removeRecursively();
            continue;
        }
        for (const QFileInfo& copy : copies) {
            Entry entry;
            entry.file = copy.absoluteFilePath();
            entry.session = session.absoluteFilePath();
            entry.saved = copy.lastModified();
            entry.title = copy.completeBaseName();
            QFile note(folder.filePath(copy.completeBaseName() + QStringLiteral(".json")));
            if (note.open(QIODevice::ReadOnly)) {
                const QJsonObject about = QJsonDocument::fromJson(note.readAll()).object();
                entry.originalPath = about.value(QStringLiteral("originalPath")).toString();
                entry.title = about.value(QStringLiteral("title")).toString(entry.title);
                const QDateTime saved = QDateTime::fromString(
                    about.value(QStringLiteral("saved")).toString(), Qt::ISODate);
                if (saved.isValid()) {
                    entry.saved = saved;
                }
            }
            entries.push_back(entry);
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.saved > b.saved; });
    return entries;
}

void RecoveryStore::removeSession(const QString& session) const {
    if (QFileInfo(session).absolutePath() != QFileInfo(m_folder).absoluteFilePath()) {
        // Only ever a folder of the recovery folder.
        return;
    }
    QDir(session).removeRecursively();
}

} // namespace lcad::app
