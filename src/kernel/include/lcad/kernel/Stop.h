// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A way to stop long work partway, whatever thread it is on. Only one piece of long work goes on
// at a time, so there is one request.
namespace lcad::kernel {

/// Asks the work going on to stop as soon as it can. Geometry the work was making comes to an
/// error, as if it had failed.
void requestStop();

/// Forgets the request, before starting more work.
void clearStop();

[[nodiscard]] bool stopRequested();

} // namespace lcad::kernel
