# Headless cross-network test kit (#3646): host/join roles over the real TLS service and GNS.
# Needs the relay-enabled GNS build (EAWR_ENABLE_RELAY) but no game data, GPU or Godot.
add_library(eawr_online_test_core STATIC options.cpp log.cpp)
target_include_directories(eawr_online_test_core PRIVATE "${PROJECT_SOURCE_DIR}/src/net")
target_link_libraries(eawr_online_test_core PUBLIC eawr::net)
target_include_directories(eawr_online_test_core PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_compile_features(eawr_online_test_core PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_online_test_core)

add_executable(eawr_online_test main.cpp run.cpp
    "${PROJECT_SOURCE_DIR}/apps/viewer/src/lan_match.cpp"
    "${PROJECT_SOURCE_DIR}/apps/viewer/src/online_match.cpp"
    "${PROJECT_SOURCE_DIR}/apps/viewer/src/skirmish_prepare.cpp"
    "${PROJECT_SOURCE_DIR}/apps/viewer/src/lan_match_fake.cpp")
# The synthetic two-player match comes from the net tests' fixtures.
target_include_directories(eawr_online_test PRIVATE
    "${PROJECT_SOURCE_DIR}/apps/viewer/src" "${PROJECT_SOURCE_DIR}/tests/net")
target_link_libraries(eawr_online_test PRIVATE eawr_online_test_core eawr::net eawr::platform eawr::skirmish eawr::ui_model)
eawr_target_project_warnings(eawr_online_test)

# Bundle the MSVC runtime app-local; target runtime discovery does not include it.
# Do not change the relay preset's CRT or its other consumers.
if(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)
endif()
set(EAWR_ONLINE_TEST_RUNTIME_DLLS "$<TARGET_RUNTIME_DLLS:eawr_online_test>;${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}")
set(EAWR_ONLINE_TEST_RUNTIME_DLLS "${EAWR_ONLINE_TEST_RUNTIME_DLLS}" PARENT_SCOPE)

# The zip for testers: the exe, its DLLs, two double-click launchers and server.conf.
set(EAWR_ONLINE_TEST_ADDRESS "" CACHE STRING "Service address written to the kit's server.conf")
set(EAWR_ONLINE_TEST_PORT "27020" CACHE STRING "Service port written to the kit's server.conf")
set(EAWR_ONLINE_TEST_TRUST "system-ca" CACHE STRING "system-ca, or pin:<sha256 of the server certificate>")
# An unset address leaves relay-only builds usable, but cannot create a package.
# A supplied placeholder fails at configure rather than producing a dead kit.
if(EAWR_ONLINE_TEST_ADDRESS STREQUAL "play.example.org")
    message(FATAL_ERROR "EAWR_ONLINE_TEST_ADDRESS must be the real service address, not play.example.org")
endif()
include("${PROJECT_SOURCE_DIR}/cmake/EawrOnlineTestConfig.cmake")
if(NOT EAWR_ONLINE_TEST_ADDRESS STREQUAL "")
    eawr_validate_online_test_config("${EAWR_ONLINE_TEST_ADDRESS}" "${EAWR_ONLINE_TEST_PORT}" "${EAWR_ONLINE_TEST_TRUST}")
endif()
add_custom_target(eawr_online_test_package
    COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:eawr_online_test>" "-DDLLS=${EAWR_ONLINE_TEST_RUNTIME_DLLS}"
        "-DPACKAGE_DIR=${CMAKE_CURRENT_LIST_DIR}/package" "-DOUT_DIR=${PROJECT_BINARY_DIR}/online-test-package"
        "-DADDRESS=${EAWR_ONLINE_TEST_ADDRESS}" "-DPORT=${EAWR_ONLINE_TEST_PORT}" "-DTRUST=${EAWR_ONLINE_TEST_TRUST}"
        -P "${PROJECT_SOURCE_DIR}/cmake/EawrOnlineTestPackage.cmake"
    DEPENDS eawr_online_test
    COMMENT "Packaging the online test kit"
    VERBATIM)
