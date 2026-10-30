cmake_minimum_required(VERSION 3.22)
project(battery_warning_test LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(CutefishToolkit REQUIRED)
find_package(GTest REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(Systemd REQUIRED IMPORTED_TARGET libsystemd)
find_program(DBUS_RUN_SESSION dbus-run-session REQUIRED)
set(SERVICE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../cutefish-services)
add_executable(battery_warning_test battery_warning_test.cpp
    ${SERVICE_DIR}/battery/battery.cpp
    ${SERVICE_DIR}/battery/batterywarning.cpp
    ${SERVICE_DIR}/notification.cpp)
target_include_directories(battery_warning_test PRIVATE ${SERVICE_DIR} ${SERVICE_DIR}/battery
    ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(battery_warning_test PRIVATE Cutefish::ToolkitBase GTest::gtest_main PkgConfig::Systemd)
enable_testing()
add_test(NAME battery_warning_test COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:battery_warning_test>)

add_executable(power_supply_test power_supply_test.cpp
    ${SERVICE_DIR}/battery/battery.cpp
    ${SERVICE_DIR}/battery/powersupply.cpp
    ${SERVICE_DIR}/log.cpp)
target_include_directories(power_supply_test PRIVATE ${SERVICE_DIR} ${SERVICE_DIR}/battery
    ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(power_supply_test PRIVATE Cutefish::ToolkitBase GTest::gtest_main)
add_test(NAME power_supply_test COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:power_supply_test>)
