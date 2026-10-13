# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(lcad_document_tests
    DocumentTest.cpp
    FileTest.cpp
    MigrationTest.cpp
    ReferenceModelTest.cpp)
target_link_libraries(lcad_document_tests PRIVATE
    lcad::core
    lcad::document
    lcad::kernel
    lcad::kernel_occt
    lcad_json
    lcad_test_support
    Catch2::Catch2WithMain)
# The archive and migration layers are internal to the module; their tests reach them directly.
target_include_directories(lcad_document_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/document/src")
target_compile_definitions(lcad_document_tests PRIVATE
    LCAD_TEST_MODELS_DIR="${PROJECT_SOURCE_DIR}/tests/models")
lcad_target_defaults(lcad_document_tests)
catch_discover_tests(lcad_document_tests PROPERTIES LABELS unit)

# Writes the reference document that tests/models/ keeps one copy of per OCCT version. See
# tests/models/README.md.
add_executable(lcad_write_reference_model WriteReferenceModel.cpp)
target_link_libraries(lcad_write_reference_model PRIVATE lcad::document lcad::kernel)
lcad_target_defaults(lcad_write_reference_model)
