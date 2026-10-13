// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Messages.h>

#include <lcad/core/Log.h>

#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Message_Printer.hxx>
#include <Message_PrinterOStream.hxx>
#include <TCollection_AsciiString.hxx>

#include <mutex>

namespace lcad::kernel {

namespace {

class LogPrinter : public Message_Printer {
    DEFINE_STANDARD_RTTI_INLINE(LogPrinter, Message_Printer)

protected:
    void send(const TCollection_AsciiString& text, const Message_Gravity gravity) const override {
        core::LogLevel level = core::LogLevel::Info;
        switch (gravity) {
        case Message_Trace:
            level = core::LogLevel::Debug;
            break;
        case Message_Info:
            level = core::LogLevel::Info;
            break;
        case Message_Warning:
            level = core::LogLevel::Warning;
            break;
        case Message_Alarm:
        case Message_Fail:
            level = core::LogLevel::Error;
            break;
        }
        core::log(level, "occt", text.ToCString());
    }
};

} // namespace

void routeMessagesToLog() {
    static std::once_flag once;
    std::call_once(once, [] {
        const Handle(Message_Messenger)& messenger = Message::DefaultMessenger();
        messenger->RemovePrinters(STANDARD_TYPE(Message_PrinterOStream));
        Handle(LogPrinter) printer = new LogPrinter();
        // OCCT filters by level itself; let everything through and leave filtering to the log.
        printer->SetTraceLevel(Message_Trace);
        messenger->AddPrinter(printer);
    });
}

} // namespace lcad::kernel
