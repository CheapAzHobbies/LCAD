# SPDX-License-Identifier: GPL-3.0-or-later

# Finds the two libraries behind the native file format and defines:
#
#   lcad_libzip   the zip container (libzip)
#   lcad_json     JSON reading and writing (nlohmann/json, header only)
#
# Both are located from their headers and libraries, like OCCT and for the same reason: the
# CMake package Ubuntu ships for libzip refers to command-line tools from other packages and
# fails to load when those are not installed.

find_path(LCAD_LIBZIP_INCLUDE_DIR NAMES zip.h DOC "Directory with libzip's zip.h")
find_library(LCAD_LIBZIP_LIBRARY NAMES zip DOC "The libzip library")
mark_as_advanced(LCAD_LIBZIP_INCLUDE_DIR LCAD_LIBZIP_LIBRARY)
if(NOT LCAD_LIBZIP_INCLUDE_DIR OR NOT LCAD_LIBZIP_LIBRARY)
    message(FATAL_ERROR
        "Could not find libzip (zip.h and the zip library). Run scripts/install-deps.sh.")
endif()

find_path(LCAD_JSON_INCLUDE_DIR NAMES nlohmann/json.hpp DOC "Directory with nlohmann/json.hpp")
mark_as_advanced(LCAD_JSON_INCLUDE_DIR)
if(NOT LCAD_JSON_INCLUDE_DIR)
    message(FATAL_ERROR
        "Could not find nlohmann/json (nlohmann/json.hpp). Run scripts/install-deps.sh.")
endif()

message(STATUS "Found libzip: ${LCAD_LIBZIP_LIBRARY}")
message(STATUS "Found nlohmann/json: ${LCAD_JSON_INCLUDE_DIR}/nlohmann/json.hpp")

add_library(lcad_libzip INTERFACE)
target_include_directories(lcad_libzip SYSTEM INTERFACE "${LCAD_LIBZIP_INCLUDE_DIR}")
target_link_libraries(lcad_libzip INTERFACE "${LCAD_LIBZIP_LIBRARY}")

add_library(lcad_json INTERFACE)
target_include_directories(lcad_json SYSTEM INTERFACE "${LCAD_JSON_INCLUDE_DIR}")
