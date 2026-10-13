// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdexcept>
#include <string>

namespace lcad::kernel {

/// A kernel operation that could not produce a result: a file that is not geometry, a boolean
/// that fails, a fillet too large for its edge. This is an expected outcome that callers handle,
/// unlike std::invalid_argument, which the kernel throws for a caller's mistake.
///
/// No exception of the underlying geometry library leaves the kernel; they arrive as this type.
class Error : public std::runtime_error {
public:
    Error(std::string operation, std::string reason);

    /// What was being attempted, for example "read geometry".
    [[nodiscard]] const std::string& operation() const noexcept { return m_operation; }
    /// Why it failed.
    [[nodiscard]] const std::string& reason() const noexcept { return m_reason; }

private:
    std::string m_operation;
    std::string m_reason;
};

} // namespace lcad::kernel
