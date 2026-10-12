# SPDX-License-Identifier: GPL-3.0-or-later

# The user interface is a library so that tests can drive it without launching the executable.
qt_add_library(lcad_app STATIC
    include/lcad/app/Application.h
    include/lcad/app/MainWindow.h
    src/Application.cpp
    src/MainWindow.cpp)
add_library(lcad::app ALIAS lcad_app)
target_include_directories(lcad_app PUBLIC include)
target_link_libraries(lcad_app
    PUBLIC Qt6::Widgets lcad::viewport
    PRIVATE lcad::kernel)
target_compile_definitions(lcad_app PRIVATE LCAD_VERSION="${PROJECT_VERSION}")
lcad_target_defaults(lcad_app)

qt_add_executable(lcad src/main.cpp)
target_link_libraries(lcad PRIVATE lcad::app)
lcad_target_defaults(lcad)
