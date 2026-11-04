cmake_minimum_required(VERSION 3.22)
project(core)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
find_package(CutefishToolkit REQUIRED)

include(GNUInstallDirs)
include_directories("${CMAKE_CURRENT_SOURCE_DIR}")

add_subdirectory(common)
add_subdirectory(appruntime)
add_subdirectory(applicationsfolder)
add_subdirectory(locales)
add_subdirectory(privacy)
add_subdirectory(session)
add_subdirectory(screenrecorder)
add_subdirectory(polkit-agent)
add_subdirectory(cutefish-services)

cutefish_add_translations(cutefish-core SOURCES cutefish-services session
                          DESKTOP_FILES session/cutefish-wayland.desktop)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cutefish-wayland.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/wayland-sessions/)
