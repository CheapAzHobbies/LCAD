# SPDX-License-Identifier: GPL-3.0-or-later

# The model: bodies, the undo history, and the native file format. No Qt and no OCCT; geometry
# is reached through the kernel only.
add_library(lcad_document STATIC
    src/Archive.cpp
    src/Document.cpp
    src/File.cpp
    src/Migration.cpp)
add_library(lcad::document ALIAS lcad_document)
target_include_directories(lcad_document PUBLIC include)
target_link_libraries(lcad_document
    PUBLIC lcad::kernel
    PRIVATE lcad::core lcad_libzip lcad_json)
lcad_target_defaults(lcad_document)
