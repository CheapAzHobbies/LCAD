# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(lcad_kernel_tests
    ErrorTest.cpp
    MeasureTest.cpp
    MessagesTest.cpp
    PrimitivesTest.cpp
    SerializationTest.cpp
    ShapeTest.cpp
    SubShapesTest.cpp
    TransformTest.cpp)
target_link_libraries(lcad_kernel_tests PRIVATE
    lcad::core
    lcad::kernel
    lcad::kernel_occt
    lcad_test_support
    Catch2::Catch2WithMain)
lcad_target_defaults(lcad_kernel_tests)
catch_discover_tests(lcad_kernel_tests PROPERTIES LABELS unit)
