# SPDX-License-Identifier: GPL-3.0-or-later

# Machining: toolpaths worked out from the document and the kernel, a simulation that cuts the
# stock with them and looks for what would go wrong, and G-code. No Qt and no OCCT.
add_library(lcad_cam STATIC
    include/lcad/cam/Gcode.h
    include/lcad/cam/Planner.h
    include/lcad/cam/Simulation.h
    include/lcad/cam/Toolpath.h
    src/Areas.cpp
    src/Areas.h
    src/Gcode.cpp
    src/Planner.cpp
    src/Simulation.cpp)
add_library(lcad::cam ALIAS lcad_cam)
target_include_directories(lcad_cam PUBLIC include)
target_link_libraries(lcad_cam PUBLIC lcad::document lcad::kernel PRIVATE lcad_clipper2)
lcad_target_defaults(lcad_cam)
# Clipper2's templates are compiled here, and Visual Studio finds unreachable code in them. Its
# copy in third_party stays as it was released.
if(MSVC)
    target_compile_options(lcad_cam PRIVATE /wd4702)
endif()
