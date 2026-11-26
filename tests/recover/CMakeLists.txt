# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(lcad_recover_tests
    RecoverTest.cpp)
target_link_libraries(lcad_recover_tests PRIVATE
    lcad::document
    lcad::kernel
    lcad::recover
    lcad_test_support
    Catch2::Catch2WithMain)
lcad_target_defaults(lcad_recover_tests)
catch_discover_tests(lcad_recover_tests PROPERTIES LABELS unit)
