// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

// LCAD's log. Every module writes to it; the application decides where messages go by adding
// sinks. With no sink installed, messages are dropped.
namespace lcad::core {

enum class LogLevel { Debug, Info, Warning, Error };

[[nodiscard]] const char* toString(LogLevel level);

struct LogMessage {
    LogLevel level = LogLevel::Info;
    /// The part of LCAD, or the library, the message comes from: "document", "occt", "qt".
    std::string source;
    std::string text;
};

using LogSink = std::function<void(const LogMessage&)>;
using LogSinkId = std::uint64_t;

/// Adds a destination for log messages and returns the handle that removes it again. Sinks may
/// be called from any thread, one message at a time.
LogSinkId addLogSink(LogSink sink);
void removeLogSink(LogSinkId id);

void log(LogLevel level, std::string_view source, std::string_view text);

} // namespace lcad::core
