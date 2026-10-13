# SPDX-License-Identifier: GPL-3.0-or-later

# The user interface is a library so that tests can drive it without launching the executable.
qt_add_library(lcad_app STATIC
    include/lcad/app/Application.h
    include/lcad/app/BrowserPanel.h
    include/lcad/app/CommandSearch.h
    include/lcad/app/Commands.h
    include/lcad/app/Icons.h
    include/lcad/app/MainWindow.h
    include/lcad/app/PrimitivePanel.h
    include/lcad/app/PropertiesPanel.h
    include/lcad/app/ShortcutsDialog.h
    include/lcad/app/Theme.h
    src/Application.cpp
    src/BrowserPanel.cpp
    src/CommandSearch.cpp
    src/Commands.cpp
    src/Icons.cpp
    src/MainWindow.cpp
    src/PrimitivePanel.cpp
    src/PropertiesPanel.cpp
    src/ShortcutsDialog.cpp
    src/Theme.cpp)
add_library(lcad::app ALIAS lcad_app)
target_include_directories(lcad_app PUBLIC include)
target_link_libraries(lcad_app
    PUBLIC Qt6::Widgets lcad::viewport lcad::document lcad::kernel
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

# What a package of LCAD contains: the program, its launcher, its icon, and the file type of
# its documents.
include(GNUInstallDirs)
set(LCAD_APP_ID io.github.cheapazhobbies.LCAD)
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
