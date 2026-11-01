# SPDX-License-Identifier: GPL-3.0-or-later

# The 3D viewport: OCCT's viewer drawing into a Qt widget. Apart from the kernel, this is the
# only module that uses OCCT directly, and its public header exposes Qt types only.
qt_add_library(lcad_viewport STATIC
    include/lcad/viewport/NavigationCube.h
    include/lcad/viewport/SketchEditor.h
    include/lcad/viewport/ViewportWidget.h
    src/NavigationCube.cpp
    src/SketchEditor.cpp
    src/ViewportWidget.cpp)
add_library(lcad::viewport ALIAS lcad_viewport)
target_include_directories(lcad_viewport PUBLIC include)
target_link_libraries(lcad_viewport
    PUBLIC Qt6::Widgets Qt6::OpenGLWidgets lcad::document lcad::kernel
    PRIVATE Qt6::OpenGL lcad::kernel_occt lcad_occt_visualization)
# wglGetCurrentDC(), for the window OCCT is handed on Windows.
if(WIN32)
    target_link_libraries(lcad_viewport PRIVATE opengl32)
endif()
lcad_target_defaults(lcad_viewport)
