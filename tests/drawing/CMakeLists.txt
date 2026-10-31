# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(lcad_drawing_tests
    SheetTest.cpp)
target_link_libraries(lcad_drawing_tests PRIVATE
    lcad::document
    lcad::drawing
    lcad::kernel
    lcad_test_support
    Catch2::Catch2WithMain)
lcad_target_defaults(lcad_drawing_tests)
catch_discover_tests(lcad_drawing_tests PROPERTIES LABELS unit)
