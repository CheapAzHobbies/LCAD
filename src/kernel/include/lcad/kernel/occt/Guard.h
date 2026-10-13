// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/kernel/Error.h>

#include <Standard_Failure.hxx>
#include <Standard_Type.hxx>

#include <string>
#include <utility>

namespace lcad::kernel::occt {

/// Runs `body` and turns any OCCT exception it throws into a kernel::Error naming `operation`.
/// Every call into OCCT that can fail goes through this.
template <typename Body>
auto guarded(const char* operation, Body&& body) -> decltype(body()) {
    try {
        return std::forward<Body>(body)();
    } catch (const Standard_Failure& failure) {
        std::string reason = failure.DynamicType()->Name();
        const char* message = failure.GetMessageString();
        if (message != nullptr && *message != '\0') {
            reason += ": ";
            reason += message;
        }
        throw Error(operation, reason);
    }
}

} // namespace lcad::kernel::occt
