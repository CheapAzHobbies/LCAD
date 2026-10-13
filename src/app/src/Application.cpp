// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/Application.h>

#include <lcad/core/Log.h>
#include <lcad/core/Version.h>
#include <lcad/kernel/Messages.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>

#include <cstdio>
#include <optional>

namespace lcad::app {

namespace {

constexpr qint64 maximumLogFileSize = 1024 * 1024;

struct Logging {
    QMutex mutex;
    QFile file;
    std::optional<core::LogSinkId> sink;
    QtMessageHandler previousQtHandler = nullptr;
};

Logging& logging() {
    static Logging instance;
    return instance;
}

void writeMessage(const core::LogMessage& message) {
    Logging& state = logging();
    const QString line =
        QStringLiteral("%1 %2 [%3] %4\n")
            .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
                 QString::fromLatin1(core::toString(message.level)).leftJustified(7),
                 QString::fromStdString(message.source), QString::fromStdString(message.text));
    const QByteArray bytes = line.toUtf8();
    const QMutexLocker lock(&state.mutex);
    if (state.file.isOpen()) {
        state.file.write(bytes);
        state.file.flush();
    }
    if (message.level >= core::LogLevel::Warning || !state.file.isOpen()) {
        std::fputs(bytes.constData(), stderr);
    }
}

void qtMessageToLog(QtMsgType type, const QMessageLogContext&, const QString& text) {
    core::LogLevel level = core::LogLevel::Info;
    switch (type) {
    case QtDebugMsg:
        level = core::LogLevel::Debug;
        break;
    case QtInfoMsg:
        level = core::LogLevel::Info;
        break;
    case QtWarningMsg:
        level = core::LogLevel::Warning;
        break;
    case QtCriticalMsg:
    case QtFatalMsg:
        level = core::LogLevel::Error;
        break;
    }
    core::log(level, "qt", text.toStdString());
}

} // namespace

QString versionString() {
    return QString::fromStdString(core::version());
}

QString defaultLogFilePath() {
    QString base = qEnvironmentVariable("XDG_STATE_HOME");
    if (base.isEmpty()) {
        base = QDir::homePath() + QStringLiteral("/.local/state");
    }
    return base + QStringLiteral("/lcad/lcad.log");
}

void startLogging(const QString& filePath) {
    stopLogging();
    Logging& state = logging();
    {
        const QMutexLocker lock(&state.mutex);
        const QFileInfo info(filePath);
        QDir().mkpath(info.absolutePath());
        if (info.exists() && info.size() > maximumLogFileSize) {
            const QString older = filePath + QStringLiteral(".1");
            QFile::remove(older);
            QFile::rename(filePath, older);
        }
        state.file.setFileName(filePath);
        if (!state.file.open(QIODevice::WriteOnly | QIODevice::Append)) {
            std::fprintf(stderr, "LCAD: cannot write the log file %s; logging to the terminal.\n",
                         qPrintable(filePath));
        }
    }
    state.sink = core::addLogSink(writeMessage);
    state.previousQtHandler = qInstallMessageHandler(qtMessageToLog);
    kernel::routeMessagesToLog();
    core::log(core::LogLevel::Info, "app", "LCAD " + core::version() + " started logging");
}

void stopLogging() {
    Logging& state = logging();
    if (!state.sink.has_value()) {
        return;
    }
    core::removeLogSink(*state.sink);
    state.sink.reset();
    qInstallMessageHandler(state.previousQtHandler);
    state.previousQtHandler = nullptr;
    const QMutexLocker lock(&state.mutex);
    state.file.close();
}

QString logFilePath() {
    Logging& state = logging();
    const QMutexLocker lock(&state.mutex);
    return state.file.isOpen() ? state.file.fileName() : QString();
}

} // namespace lcad::app
