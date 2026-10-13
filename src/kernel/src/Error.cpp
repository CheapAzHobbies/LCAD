// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Error.h>

#include <utility>

namespace lcad::kernel {

Error::Error(std::string operation, std::string reason)
    : std::runtime_error(operation + ": " + reason), m_operation(std::move(operation)),
      m_reason(std::move(reason)) {}

} // namespace lcad::kernel
