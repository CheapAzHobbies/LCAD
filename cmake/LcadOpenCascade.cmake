# Finds Open CASCADE Technology (OCCT) and defines the two targets LCAD links against:
#
#   lcad_occt_modeling        exact geometry and topology (no graphics)
#   lcad_occt_visualization   the OCCT 3D viewer, on top of lcad_occt_modeling
#
# Only the kernel and viewport modules may link these. See docs/ARCHITECTURE.md.

# OCCT's package version file uses the ExactVersion strategy, so a version passed to
# find_package() would only match that one release. Find whatever is installed and check the
# supported range below instead.
find_package(OpenCASCADE REQUIRED
    COMPONENTS FoundationClasses ModelingData ModelingAlgorithms Visualization)

set(LCAD_OCCT_VERSION
    "${OpenCASCADE_MAJOR_VERSION}.${OpenCASCADE_MINOR_VERSION}.${OpenCASCADE_MAINTENANCE_VERSION}")

if(LCAD_OCCT_VERSION VERSION_LESS "7.6")
    message(FATAL_ERROR
        "LCAD needs Open CASCADE Technology 7.6 or newer, but found ${LCAD_OCCT_VERSION} "
        "(headers in ${OpenCASCADE_INCLUDE_DIR}).")
endif()
if(LCAD_OCCT_VERSION VERSION_GREATER_EQUAL "8.0")
    message(WARNING
        "LCAD is developed and tested against Open CASCADE Technology 7.6 to 7.9. "
        "Version ${LCAD_OCCT_VERSION} changed many interfaces and may not build.")
endif()
message(STATUS "Found Open CASCADE Technology ${LCAD_OCCT_VERSION} (headers in ${OpenCASCADE_INCLUDE_DIR})")

# OCCT's imported targets carry no include directories. Add them here as SYSTEM so that OCCT's
# headers are not held to LCAD's warning flags.
add_library(lcad_occt_modeling INTERFACE)
target_include_directories(lcad_occt_modeling SYSTEM INTERFACE "${OpenCASCADE_INCLUDE_DIR}")
target_link_libraries(lcad_occt_modeling INTERFACE
    TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKGeomAlgo TKTopAlgo TKPrim TKMesh)

add_library(lcad_occt_visualization INTERFACE)
target_link_libraries(lcad_occt_visualization INTERFACE
    lcad_occt_modeling TKService TKV3d TKOpenGl)
