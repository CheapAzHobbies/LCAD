# SPDX-License-Identifier: GPL-3.0-or-later

# The user interface is a library so that tests can drive it without launching the executable.
qt_add_library(lcad_app STATIC
    include/lcad/app/Application.h
    include/lcad/app/AssemblyDialogs.h
    include/lcad/app/BrowserPanel.h
    include/lcad/app/CommandSearch.h
    include/lcad/app/Commands.h
    include/lcad/app/DrawingPanel.h
    include/lcad/app/DrawingWorkspace.h
    include/lcad/app/ExportDialog.h
    include/lcad/app/ExpressionBox.h
    include/lcad/app/FeaturePanel.h
    include/lcad/app/GeometryCheckDialog.h
    include/lcad/app/Icons.h
    include/lcad/app/JointPanel.h
    include/lcad/app/MainWindow.h
    include/lcad/app/MeasurePanel.h
    include/lcad/app/MovePanel.h
    include/lcad/app/Paths.h
    include/lcad/app/ParametersDialog.h
    include/lcad/app/PrimitivePanel.h
    include/lcad/app/PropertiesPanel.h
    include/lcad/app/Recovery.h
    include/lcad/app/SheetView.h
    include/lcad/app/ShortcutsDialog.h
    include/lcad/app/SketchPanel.h
    include/lcad/app/Theme.h
    include/lcad/app/TimelinePanel.h
    src/Application.cpp
    src/AssemblyCommands.cpp
    src/AssemblyDialogs.cpp
    src/BrowserPanel.cpp
    src/CommandSearch.cpp
    src/Commands.cpp
    src/DrawingCommands.cpp
    src/DrawingPanel.cpp
    src/DrawingWorkspace.cpp
    src/ExportDialog.cpp
    src/ExpressionBox.cpp
    src/FeaturePanel.cpp
    src/GeometryCheckDialog.cpp
    src/Icons.cpp
    src/JointPanel.cpp
    src/MainWindow.cpp
    src/MeasurePanel.cpp
    src/MovePanel.cpp
    src/Paths.cpp
    src/ParametersDialog.cpp
    src/PrimitivePanel.cpp
    src/PropertiesPanel.cpp
    src/Recovery.cpp
    src/SheetView.cpp
    src/ShortcutsDialog.cpp
    src/SketchPanel.cpp
    src/Theme.cpp
    src/TimelinePanel.cpp)
add_library(lcad::app ALIAS lcad_app)
target_include_directories(lcad_app PUBLIC include)
target_link_libraries(lcad_app
    PUBLIC Qt6::Widgets lcad::viewport lcad::document lcad::drawing lcad::kernel
    PRIVATE lcad::core)
lcad_target_defaults(lcad_app)

# The window icon is compiled in, so that it shows however LCAD was started.
qt_add_resources(lcad_app "lcad_icons"
    PREFIX "/icons"
    BASE "${PROJECT_SOURCE_DIR}/packaging"
    FILES "${PROJECT_SOURCE_DIR}/packaging/io.github.cheapazhobbies.LCAD.png")

qt_add_executable(lcad src/main.cpp)
target_link_libraries(lcad PRIVATE lcad::app lcad::core)
lcad_target_defaults(lcad)

set(LCAD_APP_ID io.github.cheapazhobbies.LCAD)
include(GNUInstallDirs)

if(WIN32)
    # A program with a window and no console, with LCAD's icon, and its version in its
    # properties.
    set(LCAD_WINDOWS_ICON "${PROJECT_SOURCE_DIR}/packaging/windows/LCAD.ico")
    configure_file("${PROJECT_SOURCE_DIR}/packaging/windows/lcad.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/lcad.rc" @ONLY)
    target_sources(lcad PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/lcad.rc")
    set_target_properties(lcad PROPERTIES WIN32_EXECUTABLE ON)

    # What the installer installs: the program in bin, beside the libraries it uses, Visual
    # C++'s runtime, which Windows does not always have, and the one plugin of Qt's that LCAD
    # needs, which Qt looks for beside the program. Qt's own windeployqt would see to the plugin,
    # but in conda-forge's Qt it cannot tell where Qt is.
    install(TARGETS lcad
        RUNTIME_DEPENDENCY_SET lcad_runtime
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(IMPORTED_RUNTIME_ARTIFACTS Qt6::QWindowsIntegrationPlugin
        RUNTIME_DEPENDENCY_SET lcad_runtime
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}/platforms"
        LIBRARY DESTINATION "${CMAKE_INSTALL_BINDIR}/platforms")
    # The libraries are found beside Qt's, and in the bin folder next to Open CASCADE's lib.
    get_target_property(_lcad_qt_core Qt6::Core IMPORTED_LOCATION_RELEASE)
    get_filename_component(_lcad_qt_bin "${_lcad_qt_core}" DIRECTORY)
    get_filename_component(_lcad_occt_lib "${LCAD_OCCT_TKernel_LIBRARY}" DIRECTORY)
    install(RUNTIME_DEPENDENCY_SET lcad_runtime
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
        # opengl32.dll belongs to Windows and the graphics driver. The one a build folder may have
        # is Mesa's, put beside the tests on a machine without a graphics card.
        PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-" "^opengl32"
        POST_EXCLUDE_REGEXES "[Ss]ystem32"
        DIRECTORIES "${_lcad_qt_bin}" "${_lcad_occt_lib}/../bin")
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "${CMAKE_INSTALL_BINDIR}")
    include(InstallRequiredSystemLibraries)
elseif(APPLE)
    # LCAD.app, which says that it opens LCAD documents.
    set(_lcad_icon "${PROJECT_SOURCE_DIR}/packaging/macos/LCAD.icns")
    target_sources(lcad PRIVATE "${_lcad_icon}")
    set_source_files_properties("${_lcad_icon}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(lcad PROPERTIES
        OUTPUT_NAME LCAD
        MACOSX_BUNDLE ON
        MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/packaging/macos/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${LCAD_APP_ID}"
        MACOSX_BUNDLE_BUNDLE_NAME LCAD
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT "GNU General Public License, version 3 or later"
        MACOSX_BUNDLE_ICON_FILE LCAD.icns)
    # macdeployqt copies into the bundle every library the program uses that macOS does not
    # have, Qt's and the others, and Qt's plugins.
    install(TARGETS lcad BUNDLE DESTINATION .)
    qt_generate_deploy_app_script(TARGET lcad
        OUTPUT_SCRIPT _lcad_deploy_script
        NO_TRANSLATIONS)
    install(SCRIPT "${_lcad_deploy_script}")
else()
    # What a package of LCAD contains: the program, its launcher, its icon, and the file type
    # of its documents.
    install(TARGETS lcad RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/${LCAD_APP_ID}.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/${LCAD_APP_ID}.svg"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/${LCAD_APP_ID}.png"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps")
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/${LCAD_APP_ID}.xml"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/mime/packages")
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/application-x-lcad.svg"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/mimetypes")
    # What software centers show about LCAD.
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/${LCAD_APP_ID}.metainfo.xml"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
endif()
