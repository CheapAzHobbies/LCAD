#pragma once

#include <string>

namespace lcad::kernel {

/// Version of Open CASCADE Technology that LCAD was built against, for example "7.9.3".
[[nodiscard]] std::string occtVersion();

} // namespace lcad::kernel
