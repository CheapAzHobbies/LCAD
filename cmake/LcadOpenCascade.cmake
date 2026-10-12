# Finds Open CASCADE Technology (OCCT) and defines the two targets LCAD links against:
#
#   lcad_occt_modeling        exact geometry and topology (no graphics)
#   lcad_occt_visualization   the OCCT 3D viewer, on top of lcad_occt_modeling
#
# Only the kernel and viewport modules may link these. See docs/ARCHITECTURE.md.
#
# OCCT is located from its headers and libraries, not through the CMake package it installs. The
# package files that distributions ship are unreliable: on Ubuntu 24.04 they link libtbb.so by
# absolute path although nothing pulls in the package that provides it, and on Ubuntu 26.04 they
# refuse to load unless OCCT's separately packaged VTK bridge is installed.
#
# To use an OCCT build from outside the system directories, add its install prefix to
# CMAKE_PREFIX_PATH.

find_path(LCAD_OCCT_INCLUDE_DIR
    NAMES Standard_Version.hxx
    PATH_SUFFIXES opencascade include/opencascade
    DOC "Directory with the Open CASCADE Technology headers")
mark_as_advanced(LCAD_OCCT_INCLUDE_DIR)
if(NOT LCAD_OCCT_INCLUDE_DIR)
    message(FATAL_ERROR
        "Could not find the Open CASCADE Technology headers (Standard_Version.hxx). "
        "Run scripts/install-deps.sh, or add the install prefix of your own OCCT build to "
        "CMAKE_PREFIX_PATH.")
endif()

file(STRINGS "${LCAD_OCCT_INCLUDE_DIR}/Standard_Version.hxx" _lcad_occt_version_lines
    REGEX "^#define OCC_VERSION_(MAJOR|MINOR|MAINTENANCE)[ \t]+[0-9]+")
foreach(_part MAJOR MINOR MAINTENANCE)
    if(NOT "${_lcad_occt_version_lines}" MATCHES "OCC_VERSION_${_part}[ \t]+([0-9]+)")
        message(FATAL_ERROR
            "Could not read the OCCT version from ${LCAD_OCCT_INCLUDE_DIR}/Standard_Version.hxx.")
    endif()
    set(_lcad_occt_${_part} "${CMAKE_MATCH_1}")
endforeach()
set(LCAD_OCCT_VERSION "${_lcad_occt_MAJOR}.${_lcad_occt_MINOR}.${_lcad_occt_MAINTENANCE}")

if(LCAD_OCCT_VERSION VERSION_LESS "7.6")
    message(FATAL_ERROR
        "LCAD needs Open CASCADE Technology 7.6 or newer, but found ${LCAD_OCCT_VERSION} "
        "(headers in ${LCAD_OCCT_INCLUDE_DIR}).")
endif()
if(LCAD_OCCT_VERSION VERSION_GREATER_EQUAL "8.0")
    message(WARNING
        "LCAD is developed and tested against Open CASCADE Technology 7.6 to 7.9. "
        "Version ${LCAD_OCCT_VERSION} changed many interfaces and may not build.")
endif()

# Sets <result> to the full paths of the named OCCT toolkit libraries.
function(lcad_find_occt_toolkits result)
    set(_libraries "")
    set(_missing "")
    foreach(_toolkit IN LISTS ARGN)
        find_library(LCAD_OCCT_${_toolkit}_LIBRARY
            NAMES ${_toolkit}
            DOC "Open CASCADE Technology toolkit ${_toolkit}")
        mark_as_advanced(LCAD_OCCT_${_toolkit}_LIBRARY)
        if(LCAD_OCCT_${_toolkit}_LIBRARY)
            list(APPEND _libraries "${LCAD_OCCT_${_toolkit}_LIBRARY}")
        else()
            list(APPEND _missing ${_toolkit})
        endif()
    endforeach()
    if(_missing)
        list(JOIN _missing ", " _missing_names)
        message(FATAL_ERROR
            "Found the Open CASCADE Technology ${LCAD_OCCT_VERSION} headers in "
            "${LCAD_OCCT_INCLUDE_DIR}, but not these libraries: ${_missing_names}. "
            "Run scripts/install-deps.sh, or add the install prefix of your own OCCT build to "
            "CMAKE_PREFIX_PATH.")
    endif()
    set(${result} "${_libraries}" PARENT_SCOPE)
endfunction()

lcad_find_occt_toolkits(_lcad_occt_modeling_libraries
    TKPrim TKTopAlgo TKGeomAlgo TKMesh TKBRep TKGeomBase TKG3d TKG2d TKMath TKernel)
lcad_find_occt_toolkits(_lcad_occt_visualization_libraries
    TKOpenGl TKV3d TKService)

message(STATUS
    "Found Open CASCADE Technology ${LCAD_OCCT_VERSION} (headers in ${LCAD_OCCT_INCLUDE_DIR})")

# SYSTEM, so that OCCT's headers are not held to LCAD's warning flags.
add_library(lcad_occt_modeling INTERFACE)
target_include_directories(lcad_occt_modeling SYSTEM INTERFACE "${LCAD_OCCT_INCLUDE_DIR}")
target_link_libraries(lcad_occt_modeling INTERFACE ${_lcad_occt_modeling_libraries})

add_library(lcad_occt_visualization INTERFACE)
target_link_libraries(lcad_occt_visualization INTERFACE
    ${_lcad_occt_visualization_libraries} lcad_occt_modeling)
