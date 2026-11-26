# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(lcad_meshprobe meshprobe.cpp)
target_link_libraries(lcad_meshprobe PRIVATE lcad::kernel)
lcad_target_defaults(lcad_meshprobe)

add_executable(lcad_recoverprobe recoverprobe.cpp)
target_link_libraries(lcad_recoverprobe PRIVATE lcad::recover)
lcad_target_defaults(lcad_recoverprobe)
