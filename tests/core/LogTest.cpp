// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/core/Log.h>
#include <lcad/core/Version.h>

#include <catch2/catch_test_macros.hpp>

#include <regex>
#include <vector>

using namespace lcad::core;

TEST_CASE("A log message reaches every sink until the sink is removed", "[core][log]") {
    std::vector<LogMessage> first;
    std::vector<LogMessage> second;
    const LogSinkId firstId = addLogSink([&](const LogMessage& m) { first.push_back(m); });
    const LogSinkId secondId = addLogSink([&](const LogMessage& m) { second.push_back(m); });

    log(LogLevel::Warning, "document", "something odd");

    REQUIRE(first.size() == 1);
    CHECK(first[0].level == LogLevel::Warning);
    CHECK(first[0].source == "document");
    CHECK(first[0].text == "something odd");
    CHECK(second.size() == 1);

    removeLogSink(firstId);
    log(LogLevel::Error, "kernel", "worse");

    CHECK(first.size() == 1);
    REQUIRE(second.size() == 2);
    CHECK(second[1].level == LogLevel::Error);

    removeLogSink(secondId);
    log(LogLevel::Info, "kernel", "nobody listens");
    CHECK(second.size() == 2);
}

TEST_CASE("A sink that logs does not recurse", "[core][log]") {
    int calls = 0;
    const LogSinkId id = addLogSink([&](const LogMessage&) {
        ++calls;
        log(LogLevel::Error, "sink", "failed to write");
    });

    log(LogLevel::Info, "test", "once");

    CHECK(calls == 1);
    removeLogSink(id);
}

TEST_CASE("Log levels have names", "[core][log]") {
    CHECK(std::string(toString(LogLevel::Debug)) == "debug");
    CHECK(std::string(toString(LogLevel::Info)) == "info");
    CHECK(std::string(toString(LogLevel::Warning)) == "warning");
    CHECK(std::string(toString(LogLevel::Error)) == "error");
}

TEST_CASE("LCAD's version is three numbers", "[core]") {
    CHECK(std::regex_match(version(), std::regex(R"(\d+\.\d+\.\d+)")));
}
