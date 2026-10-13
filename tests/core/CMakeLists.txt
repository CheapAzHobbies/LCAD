# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(lcad_core_tests
    LogTest.cpp)
target_link_libraries(lcad_core_tests PRIVATE lcad::core Catch2::Catch2WithMain)
lcad_target_defaults(lcad_core_tests)
catch_discover_tests(lcad_core_tests PROPERTIES LABELS unit)
