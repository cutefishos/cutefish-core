cmake_minimum_required(VERSION 3.22)
project(softwareupdate_test LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(CutefishToolkit REQUIRED)
find_package(CutefishClients REQUIRED COMPONENTS packagekit)
find_package(GTest REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(Systemd REQUIRED IMPORTED_TARGET libsystemd)
find_program(DBUS_RUN_SESSION dbus-run-session REQUIRED)
set(SERVICE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../cutefish-services)
add_executable(softwareupdate_test softwareupdate_test.cpp
    ${SERVICE_DIR}/softwareupdate/softwareupdate.cpp
    ${SERVICE_DIR}/notification.cpp
    ${SERVICE_DIR}/log.cpp)
target_include_directories(softwareupdate_test PRIVATE ${SERVICE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(softwareupdate_test PRIVATE Cutefish::ToolkitBase Cutefish::PackageKitClient GTest::gtest_main
    PkgConfig::Systemd)
enable_testing()
# A session bus without service files: some desktops install a session
# service under PackageKit's name, which would answer the absent daemon's case.
add_test(NAME softwareupdate_test COMMAND ${DBUS_RUN_SESSION} --config-file=${CMAKE_CURRENT_SOURCE_DIR}/dbus-session.conf
    -- $<TARGET_FILE:softwareupdate_test>)
