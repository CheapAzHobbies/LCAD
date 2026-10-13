# SPDX-License-Identifier: GPL-3.0-or-later

# What every other module may use: the log and LCAD's version. No Qt, no OCCT.
add_library(lcad_core STATIC
    src/Log.cpp
    src/Version.cpp)
add_library(lcad::core ALIAS lcad_core)
target_include_directories(lcad_core PUBLIC include)
target_compile_definitions(lcad_core PRIVATE LCAD_VERSION="${PROJECT_VERSION}")
lcad_target_defaults(lcad_core)
