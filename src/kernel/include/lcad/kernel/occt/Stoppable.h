// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/kernel/Stop.h>

#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressScope.hxx>

namespace lcad::kernel::occt {

/// What an OCCT algorithm is given for it to look up from its work now and then, and stop if
/// kernel::requestStop() was called. It says that it did by not being done, or by errors.
///
///     const Handle(occt::Stoppable) stoppable = new occt::Stoppable();
///     maker.Build(stoppable->Start());
class Stoppable final : public Message_ProgressIndicator {
public:
    Standard_Boolean UserBreak() override { return stopRequested(); }
    void Show(const Message_ProgressScope&, const Standard_Boolean) override {}
};

} // namespace lcad::kernel::occt
