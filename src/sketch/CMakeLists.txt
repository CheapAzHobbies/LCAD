# SPDX-License-Identifier: GPL-3.0-or-later

# 2D sketches: points, lines, circles, and arcs on a plane, the constraints between them, and
# the solver that moves the geometry to satisfy the constraints. No Qt and no OCCT; Eigen is
# used inside the solver only.
add_library(lcad_sketch STATIC
    src/Profiles.cpp
    src/Sketch.cpp
    src/Solver.cpp)
add_library(lcad::sketch ALIAS lcad_sketch)
target_include_directories(lcad_sketch PUBLIC include)
target_link_libraries(lcad_sketch PRIVATE lcad_eigen)
lcad_target_defaults(lcad_sketch)
