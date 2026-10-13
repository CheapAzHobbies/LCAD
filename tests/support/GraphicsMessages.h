// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/core/Log.h>
#include <lcad/kernel/Messages.h>

#include <QString>
#include <QStringList>

namespace lcad::test {

/// Collects what OCCT's viewer complains about while a test runs: every warning and error it
/// sends, and every OpenGL error it notices, which it reports at a low level and carries on.
/// An interface test fails if this is not empty at its end.
class GraphicsMessages {
public:
    GraphicsMessages() {
        kernel::routeMessagesToLog();
        m_sink = core::addLogSink([this](const core::LogMessage& message) {
            if (message.source != "occt") {
                return;
            }
            const QString text = QString::fromStdString(message.text);
            if (message.level >= core::LogLevel::Warning ||
                text.contains(QStringLiteral("GL error"))) {
                m_messages << text.simplified();
            }
        });
    }
    ~GraphicsMessages() { core::removeLogSink(m_sink); }
    GraphicsMessages(const GraphicsMessages&) = delete;
    GraphicsMessages& operator=(const GraphicsMessages&) = delete;

    [[nodiscard]] QStringList take() {
        QStringList messages;
        messages.swap(m_messages);
        return messages;
    }

private:
    core::LogSinkId m_sink = 0;
    QStringList m_messages;
};

} // namespace lcad::test
