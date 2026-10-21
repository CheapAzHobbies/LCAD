# SPDX-License-Identifier: GPL-3.0-or-later

# What every other module may use: the log, LCAD's version, and arithmetic expressions. No Qt,
# no OCCT.
add_library(lcad_core STATIC
    src/Expression.cpp
    src/Log.cpp
    src/Path.cpp
    src/Version.cpp)
add_library(lcad::core ALIAS lcad_core)
target_include_directories(lcad_core PUBLIC include)
target_compile_definitions(lcad_core PRIVATE LCAD_VERSION="${PROJECT_VERSION}")
lcad_target_defaults(lcad_core)
