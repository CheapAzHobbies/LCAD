# SPDX-License-Identifier: GPL-3.0-or-later

# Turns a mesh into a model: from the surfaces of the mesh it works out sketches and the features
# that make it, and failing that, a solid. No Qt and no OCCT.
add_library(lcad_recover STATIC
    include/lcad/recover/Recover.h
    src/MeshFacts.cpp
    src/MeshFacts.h
    src/Plan.cpp
    src/Plan.h
    src/Recover.cpp
    src/Sharpen.cpp
    src/Sharpen.h)
add_library(lcad::recover ALIAS lcad_recover)
target_include_directories(lcad_recover PUBLIC include)
target_link_libraries(lcad_recover PUBLIC lcad::document lcad::kernel PRIVATE lcad_eigen)
lcad_target_defaults(lcad_recover)
