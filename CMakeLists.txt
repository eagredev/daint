cmake_minimum_required(VERSION 3.16)

project(daint VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt's meta-object compiler (moc) etc. run automatically.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# DBus: the "Pick from screen" eyedropper talks to the XDG desktop portal.
find_package(Qt6 REQUIRED COMPONENTS Widgets DBus)

add_executable(daint
    src/main.cpp
    src/canvas.cpp
    src/mainwindow.cpp
    src/coloreditor.cpp
    src/ruler.cpp
)

target_link_libraries(daint PRIVATE Qt6::Widgets Qt6::DBus)

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
