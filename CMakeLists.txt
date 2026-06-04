cmake_minimum_required(VERSION 3.16)

project(daint VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt's meta-object compiler (moc) etc. run automatically.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# The "Pick from screen" eyedropper talks to the XDG desktop portal over DBus,
# which only exists on Linux/Unix. Require (and link) Qt6::DBus only there; on
# other platforms (e.g. Windows) the feature is compiled out via the
# DAINT_HAS_SCREEN_PICKER macro and the dependency isn't needed.
if(UNIX AND NOT APPLE)
    set(DAINT_SCREEN_PICKER ON)
endif()

if(DAINT_SCREEN_PICKER)
    find_package(Qt6 REQUIRED COMPONENTS Widgets DBus)
else()
    find_package(Qt6 REQUIRED COMPONENTS Widgets)
endif()

add_executable(daint
    src/main.cpp
    src/canvas.cpp
    src/mainwindow.cpp
    src/coloreditor.cpp
    src/ruler.cpp
)

target_link_libraries(daint PRIVATE Qt6::Widgets)

if(DAINT_SCREEN_PICKER)
    target_link_libraries(daint PRIVATE Qt6::DBus)
    target_compile_definitions(daint PRIVATE DAINT_HAS_SCREEN_PICKER)
endif()

# --- Installation (used by the Flatpak build) ---------------------------------
include(GNUInstallDirs)

install(TARGETS daint RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES data/io.github.eagredev.Daint.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)

install(FILES data/io.github.eagredev.Daint.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo)

# Scalable app icon. Freedesktop/Flathub convention: a single SVG under
# hicolor/scalable/apps named exactly after the app-id, which is what the
# desktop file's Icon= key and the AppStream id both reference.
install(FILES data/icons/io.github.eagredev.Daint.svg
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps)
