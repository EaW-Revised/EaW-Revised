# eawr_mods_service: the mod manager's local stdio service (docs/mods-service-protocol.md). The launcher
# repository's Godot manager starts it through its stdio pipes; on Windows it is a GUI-subsystem program, so it
# never opens a console window.
#
# Compile shared portable helpers in this target so all objects use the static CRT.
include("${PROJECT_SOURCE_DIR}/src/mods/sources.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/EawrBuildIdentity.cmake")
set(EAWR_MODS_SERVICE_SOURCES
    src/line_framer.cpp
    src/loadcheck.cpp
    src/methods.cpp
    src/model_methods.cpp
    src/service.cpp
    ${EAWR_MODS_SOURCES}
    "${PROJECT_SOURCE_DIR}/apps/launcher/src/install_discovery.cpp"
    "${PROJECT_SOURCE_DIR}/apps/launcher/src/launcher_state.cpp"
    "${PROJECT_SOURCE_DIR}/src/core/sha256.cpp"
    "${PROJECT_SOURCE_DIR}/src/core/json_text.cpp"
    "${EAWR_BUILD_IDENTITY_CPP}"
)

add_executable(eawr_mods_service WIN32 src/main.cpp ${EAWR_MODS_SERVICE_SOURCES})
target_include_directories(eawr_mods_service PRIVATE src "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/apps/launcher/src")
target_compile_definitions(eawr_mods_service PRIVATE EAWR_MODS_SERVICE_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_compile_definitions(eawr_mods_service PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_sources(eawr_mods_service PRIVATE "${PROJECT_SOURCE_DIR}/apps/launcher/src/win_registry.cpp")
    target_link_libraries(eawr_mods_service PRIVATE shell32 advapi32 ole32 uuid)
endif()
set_target_properties(eawr_mods_service PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_features(eawr_mods_service PRIVATE cxx_std_20)
eawr_target_project_warnings(eawr_mods_service)
