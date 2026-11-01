// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

#include <functional>

namespace lcad::app {

/// Runs work that may take a while, and returns when it is done, with whatever it threw.
///
/// The work runs on a thread of its own, with a far larger stack than a thread gets by default,
/// which deep geometry needs. Meanwhile the window stays alive, though it takes no input. Past a
/// moment a small box says what is going on and how long it has taken, and has a Stop button.
/// There is no limit to how long it can go on: the user decides. Stop asks through
/// kernel::requestStop(), and the work ends as soon as it next looks up. Should it not, the box
/// offers to wait on, or to keep what can be kept and close LCAD.
///
/// A fault in the work, such as a wild pointer in the geometry library or the stack running out,
/// comes out as a kernel::Error, not as the end of the program. What was kept by
/// setLongWorkLastResort() is kept then too, since things may not be sound after that.
///
/// Called from within long work, or from another thread, the work is simply done there.
void runLongWork(const QString& name, const std::function<void()>& work);

/// Has the document's long work go through runLongWork().
void installLongWork();

/// What to do if work that was asked to stop never does, before LCAD closes, or if it faults.
void setLongWorkLastResort(std::function<void()> keepWhatCanBeKept);

/// How long work has to stop before the box says it has not, in milliseconds.
void setLongWorkPatience(int milliseconds);

} // namespace lcad::app
