openmw_add_executable(openmw-diagnosticctl
    main.cpp
    controllerprotocol.cpp controllerprotocol.hpp
    gameplayrouteexecution.cpp gameplayrouteexecution.hpp
    isolatedoptions.cpp isolatedoptions.hpp
    isolatedsettings.cpp isolatedsettings.hpp
    isolatedrun.cpp isolatedrun.hpp
    isolatedworkspace.cpp isolatedworkspace.hpp
    replaystate.cpp replaystate.hpp
    settingssequences.cpp settingssequences.hpp
)

target_link_libraries(openmw-diagnosticctl components)

if (WIN32)
    target_sources(openmw-diagnosticctl PRIVATE ${CMAKE_SOURCE_DIR}/files/windows/other-apps.manifest)
endif()

install(TARGETS openmw-diagnosticctl RUNTIME DESTINATION "${BINDIR}")
