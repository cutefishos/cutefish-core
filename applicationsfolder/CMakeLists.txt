project(cutefish-applications-folder)
set(TARGET cutefish-applications-folder)

set(SOURCES
    applicationsfolderservice.cpp
    applicationsfolderservice.h
    main.cpp
)

add_executable(${TARGET} ${SOURCES})
target_link_libraries(${TARGET}
    Cutefish::ToolkitBase
    cutefish-core-common
)

configure_file(org.cutefish.ApplicationsFolder.service.in org.cutefish.ApplicationsFolder.service @ONLY)
configure_file(cutefish-applications-folder.service.in cutefish-applications-folder.service @ONLY)

install(TARGETS ${TARGET} DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
install(FILES org.cutefish.ApplicationsFolder.conf DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/dbus-1/system.d)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.cutefish.ApplicationsFolder.service
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/dbus-1/system-services)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cutefish-applications-folder.service DESTINATION lib/systemd/system)
install(FILES org.cutefish.applications-folder.policy DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/polkit-1/actions)
