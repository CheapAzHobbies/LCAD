# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(lcad_kernel_tests
    MeshTest.cpp
    DirectEditTest.cpp
    ErrorTest.cpp
    ExchangeTest.cpp
    MeasureTest.cpp
    ModelingTest.cpp
    MessagesTest.cpp
    PlaneTest.cpp
    PrimitivesTest.cpp
    ProjectionTest.cpp
    StopTest.cpp
    SerializationTest.cpp
    ShapeTest.cpp
    SubShapesTest.cpp
    TransformTest.cpp)
target_link_libraries(lcad_kernel_tests PRIVATE
    lcad::core
    lcad::kernel
    lcad::kernel_occt
    # ExchangeTest writes files in other ways than LCAD does, and opens 3MF packages.
    lcad_occt_exchange
    lcad_libzip
    lcad_test_support
    Catch2::Catch2WithMain)
lcad_target_defaults(lcad_kernel_tests)
# The STEP and IGES translators leave global tables behind, which tests/lsan.supp names. Only has
# an effect in sanitizer builds.
catch_discover_tests(lcad_kernel_tests PROPERTIES
    LABELS unit
    ENVIRONMENT "LSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/tests/lsan.supp:print_suppressions=0")
