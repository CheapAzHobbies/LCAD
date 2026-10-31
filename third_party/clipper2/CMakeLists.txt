# SPDX-License-Identifier: BSL-1.0

# Clipper2, polygon clipping and offsetting, the offset paths of milling come from. See README.md.
add_library(lcad_clipper2 STATIC
    src/clipper.engine.cpp
    src/clipper.offset.cpp
    src/clipper.rectclip.cpp)
# SYSTEM: its headers are not held to LCAD's warning flags.
target_include_directories(lcad_clipper2 SYSTEM PUBLIC include)
target_compile_features(lcad_clipper2 PUBLIC cxx_std_17)
target_compile_definitions(lcad_clipper2 PUBLIC CLIPPER2_MAX_DECIMAL_PRECISION=8)
set_target_properties(lcad_clipper2 PROPERTIES POSITION_INDEPENDENT_CODE ON)
