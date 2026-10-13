// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/kernel/Shape.h>

#include <string>
#include <string_view>

namespace lcad::kernel {

// Exact geometry as bytes, for LCAD's native file format. The encoding is the kernel library's
// binary B-rep format at one pinned version, so that bytes written by any supported version of
// the library can be read by every other. Display meshes are not included.

/// The bytes for `shape`. The std::string is a byte container, not text.
///
/// Throws std::invalid_argument for a null shape, and kernel::Error if writing fails.
[[nodiscard]] std::string toBrep(const Shape& shape);

/// The shape stored in `bytes`. Throws kernel::Error when the bytes are not a complete shape
/// in this format.
[[nodiscard]] Shape fromBrep(std::string_view bytes);

} // namespace lcad::kernel
