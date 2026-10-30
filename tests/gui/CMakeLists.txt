# SPDX-License-Identifier: GPL-3.0-or-later

# GUI tests run under xvfb-run when it is available: each gets a private X server and Mesa's
# software renderer, so results are the same on a desktop and in CI, and no window ever appears
# on a real screen.
find_program(LCAD_XVFB_RUN xvfb-run DOC "Runs GUI tests on a virtual X display")
if(NOT LCAD_XVFB_RUN)
    message(WARNING "xvfb-run was not found; GUI tests will open windows on the current display.")
endif()

function(lcad_add_gui_test name)
    qt_add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE
        Qt6::Test lcad::app lcad::core lcad::document lcad::kernel lcad_test_support)
    lcad_target_defaults(${name})

    # On Linux, force X11 so the test never lands on a developer's Wayland session, and software
    # rendering so the result does not depend on the machine's GPU. LSAN_OPTIONS only has an
    # effect in sanitizer builds. Windows and macOS use their own platform plugin and driver.
    set(environment
        "LSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/tests/lsan.supp:print_suppressions=0")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND environment "QT_QPA_PLATFORM=xcb" "LIBGL_ALWAYS_SOFTWARE=1")
    endif()

    # Each program runs twice: at the screen's own scale, and at twice that, as on a high-DPI
    # display, where a logical pixel is two physical ones. The second run wants a screen made
    # for it: on the small screens of CI's Windows and macOS machines, twice the scale would
    # leave no room for the window, and those run once.
    if(LCAD_XVFB_RUN)
        add_test(NAME ${name}
            COMMAND "${LCAD_XVFB_RUN}" --auto-servernum "--server-args=-screen 0 1700x1100x24"
                    $<TARGET_FILE:${name}>)
        add_test(NAME ${name}_2x
            COMMAND "${LCAD_XVFB_RUN}" --auto-servernum "--server-args=-screen 0 3400x2200x24"
                    $<TARGET_FILE:${name}>)
        set_tests_properties(${name}_2x PROPERTIES
            LABELS gui ENVIRONMENT "${environment};QT_SCALE_FACTOR=2")
    else()
        add_test(NAME ${name} COMMAND ${name})
    endif()
    set_tests_properties(${name} PROPERTIES LABELS gui ENVIRONMENT "${environment}")
endfunction()

lcad_add_gui_test(lcad_viewport_tests ViewportTest.cpp)
lcad_add_gui_test(lcad_sketch_view_tests SketchViewTest.cpp)
lcad_add_gui_test(lcad_app_tests ApplicationTest.cpp)
lcad_add_gui_test(lcad_interface_tests InterfaceTest.cpp)
lcad_add_gui_test(lcad_sketch_interface_tests SketchInterfaceTest.cpp)
lcad_add_gui_test(lcad_modeling_interface_tests ModelingInterfaceTest.cpp)
lcad_add_gui_test(lcad_interchange_interface_tests InterchangeInterfaceTest.cpp)
lcad_add_gui_test(lcad_assembly_interface_tests AssemblyInterfaceTest.cpp)
# ApplicationTest asks the X server which kind of server it is.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(X11 REQUIRED)
    target_link_libraries(lcad_app_tests PRIVATE X11::xcb)
endif()

# Needs no display, so it runs with the unit tests.
qt_add_executable(lcad_viewport_setup_tests SetupTest.cpp)
target_link_libraries(lcad_viewport_setup_tests PRIVATE Qt6::Test lcad::viewport)
lcad_target_defaults(lcad_viewport_setup_tests)
add_test(NAME lcad_viewport_setup_tests COMMAND lcad_viewport_setup_tests)
set_tests_properties(lcad_viewport_setup_tests PROPERTIES LABELS unit)
