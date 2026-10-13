// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace lcad::kernel {

/// Sends the geometry library's diagnostic messages to LCAD's log (source "occt") and stops it
/// from printing them to the terminal. Safe to call more than once.
void routeMessagesToLog();

} // namespace lcad::kernel
