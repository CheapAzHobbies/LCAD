// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/core/Log.h>
#include <lcad/kernel/Messages.h>

#include <Message.hxx>
#include <Message_Messenger.hxx>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace lcad;

TEST_CASE("OCCT's diagnostic messages arrive in LCAD's log", "[kernel][messages]") {
    std::vector<core::LogMessage> received;
    const core::LogSinkId sink =
        core::addLogSink([&](const core::LogMessage& m) { received.push_back(m); });

    kernel::routeMessagesToLog();
    kernel::routeMessagesToLog(); // a second call must not deliver everything twice

    Message::DefaultMessenger()->Send("mesh quality is poor", Message_Warning);
    Message::DefaultMessenger()->Send("cannot build face", Message_Fail);
    Message::DefaultMessenger()->Send("progress note", Message_Info);

    core::removeLogSink(sink);

    REQUIRE(received.size() == 3);
    CHECK(received[0].source == "occt");
    CHECK(received[0].level == core::LogLevel::Warning);
    CHECK(received[0].text == "mesh quality is poor");
    CHECK(received[1].level == core::LogLevel::Error);
    CHECK(received[1].text == "cannot build face");
    CHECK(received[2].level == core::LogLevel::Info);
}
