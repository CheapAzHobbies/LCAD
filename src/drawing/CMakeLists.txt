# SPDX-License-Identifier: GPL-3.0-or-later

# Engineering drawings: what a sheet shows, worked out from the document and the kernel, and
# DXF files. No Qt and no OCCT; a sheet is a list of lines and labels that the interface paints.
add_library(lcad_drawing STATIC
    include/lcad/drawing/Dxf.h
    include/lcad/drawing/Sheet.h
    src/Dxf.cpp
    src/Sheet.cpp)
add_library(lcad::drawing ALIAS lcad_drawing)
target_include_directories(lcad_drawing PUBLIC include)
target_link_libraries(lcad_drawing PUBLIC lcad::document lcad::kernel PRIVATE lcad::core)
lcad_target_defaults(lcad_drawing)
