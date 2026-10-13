// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/core/Log.h>

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

namespace lcad::core {

namespace {

struct Registry {
    // Recursive, so that a sink which itself logs does not deadlock.
    std::recursive_mutex mutex;
    std::vector<std::pair<LogSinkId, LogSink>> sinks;
    LogSinkId nextId = 1;
    bool delivering = false;
};

Registry& registry() {
    static Registry instance;
    return instance;
}

} // namespace

const char* toString(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warning:
        return "warning";
    case LogLevel::Error:
        return "error";
    }
    return "unknown";
}

LogSinkId addLogSink(LogSink sink) {
    Registry& r = registry();
    const std::lock_guard lock(r.mutex);
    const LogSinkId id = r.nextId++;
    r.sinks.emplace_back(id, std::move(sink));
    return id;
}

void removeLogSink(LogSinkId id) {
    Registry& r = registry();
    const std::lock_guard lock(r.mutex);
    std::erase_if(r.sinks, [id](const auto& entry) { return entry.first == id; });
}

void log(LogLevel level, std::string_view source, std::string_view text) {
    Registry& r = registry();
    const std::lock_guard lock(r.mutex);
    if (r.delivering) {
        // A sink logged while handling a message. Passing that on could recurse without end.
        return;
    }
    r.delivering = true;
    const LogMessage message{level, std::string(source), std::string(text)};
    // Indexed, and re-checked each round: a sink is allowed to remove itself.
    for (std::size_t i = 0; i < r.sinks.size(); ++i) {
        const LogSink sink = r.sinks[i].second;
        sink(message);
    }
    r.delivering = false;
}

} // namespace lcad::core
