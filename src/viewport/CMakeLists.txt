# The 3D viewport: OCCT's viewer drawing into a Qt widget. Apart from the kernel, this is the
# only module that uses OCCT directly, and its public header exposes Qt types only.
qt_add_library(lcad_viewport STATIC
    include/lcad/viewport/ViewportWidget.h
    src/ViewportWidget.cpp)
add_library(lcad::viewport ALIAS lcad_viewport)
target_include_directories(lcad_viewport PUBLIC include)
target_link_libraries(lcad_viewport
    PUBLIC Qt6::Widgets Qt6::OpenGLWidgets lcad::kernel
    PRIVATE Qt6::OpenGL lcad::kernel_occt lcad_occt_visualization)
lcad_target_defaults(lcad_viewport)
