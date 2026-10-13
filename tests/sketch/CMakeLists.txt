# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(lcad_sketch_tests
    EditTest.cpp
    ProfilesTest.cpp
    SketchTest.cpp
    SolverTest.cpp)
target_link_libraries(lcad_sketch_tests PRIVATE lcad::sketch lcad_test_support Catch2::Catch2WithMain)
lcad_target_defaults(lcad_sketch_tests)
catch_discover_tests(lcad_sketch_tests PROPERTIES LABELS unit)
