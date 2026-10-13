# SPDX-License-Identifier: GPL-3.0-or-later

# The user interface is a library so that tests can drive it without launching the executable.
qt_add_library(lcad_app STATIC
    include/lcad/app/Application.h
    include/lcad/app/BrowserPanel.h
    include/lcad/app/MainWindow.h
    include/lcad/app/PrimitivePanel.h
    src/Application.cpp
    src/BrowserPanel.cpp
    src/MainWindow.cpp
    src/PrimitivePanel.cpp)
add_library(lcad::app ALIAS lcad_app)
target_include_directories(lcad_app PUBLIC include)
target_link_libraries(lcad_app
    PUBLIC Qt6::Widgets lcad::viewport lcad::document lcad::kernel
    PRIVATE lcad::core)
lcad_target_defaults(lcad_app)

qt_add_executable(lcad src/main.cpp)
target_link_libraries(lcad PRIVATE lcad::app)
lcad_target_defaults(lcad)
