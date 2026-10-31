# SPDX-License-Identifier: GPL-3.0-or-later

# Exact geometry. The public headers expose no OCCT types, so OCCT is a private dependency.
add_library(lcad_kernel STATIC
    src/Check.cpp
    src/Error.cpp
    src/Exchange.cpp
    src/Measure.cpp
    src/Messages.cpp
    src/Modeling.cpp
    src/Plane.cpp
    src/Primitives.cpp
    src/Projection.cpp
    src/Serialization.cpp
    src/Shape.cpp
    src/SubShapes.cpp
    src/Transform.cpp
    src/Version.cpp)
add_library(lcad::kernel ALIAS lcad_kernel)
target_include_directories(lcad_kernel PUBLIC include)
# Exchange.cpp writes 3MF files, which are zip archives.
target_link_libraries(lcad_kernel PRIVATE lcad::core lcad_occt_exchange lcad_libzip)
lcad_target_defaults(lcad_kernel)

# For the few modules that have to hand shapes to OCCT themselves (the viewport, kernel tests).
# Linking this is what makes lcad/kernel/occt/ShapeAccess.h and the OCCT headers usable.
add_library(lcad_kernel_occt INTERFACE)
add_library(lcad::kernel_occt ALIAS lcad_kernel_occt)
target_link_libraries(lcad_kernel_occt INTERFACE lcad_kernel lcad_occt_modeling)
