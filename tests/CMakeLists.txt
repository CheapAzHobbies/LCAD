# SPDX-License-Identifier: GPL-3.0-or-later

find_package(Catch2 3 REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Test)
include(Catch)

add_library(lcad_test_support INTERFACE)
target_include_directories(lcad_test_support INTERFACE support)

add_subdirectory(core)
add_subdirectory(kernel)
add_subdirectory(gui)
