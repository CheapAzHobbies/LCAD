# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(lcad_meshprobe meshprobe.cpp)
target_link_libraries(lcad_meshprobe PRIVATE lcad::kernel)
lcad_target_defaults(lcad_meshprobe)

add_executable(lcad_meshwhy meshwhy.cpp)
target_link_libraries(lcad_meshwhy PRIVATE lcad::kernel lcad::kernel_occt)
lcad_target_defaults(lcad_meshwhy)
