openmw_add_executable(openmw-diagnosticctl main.cpp)

target_link_libraries(openmw-diagnosticctl components)

if (WIN32)
    target_sources(openmw-diagnosticctl PRIVATE ${CMAKE_SOURCE_DIR}/files/windows/other-apps.manifest)
endif()

install(TARGETS openmw-diagnosticctl RUNTIME DESTINATION "${BINDIR}")
