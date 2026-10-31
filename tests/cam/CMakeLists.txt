# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(lcad_cam_tests
    CamTest.cpp)
target_link_libraries(lcad_cam_tests PRIVATE
    lcad::cam
    lcad::document
    lcad::kernel
    lcad_test_support
    Catch2::Catch2WithMain)
lcad_target_defaults(lcad_cam_tests)
catch_discover_tests(lcad_cam_tests PROPERTIES LABELS unit)
