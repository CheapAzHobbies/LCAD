// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Version.h>

#include <Standard_Version.hxx>

namespace lcad::kernel {

std::string occtVersion() {
    return OCC_VERSION_COMPLETE;
}

} // namespace lcad::kernel
