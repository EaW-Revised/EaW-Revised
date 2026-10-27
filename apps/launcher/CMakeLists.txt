# EAWR.exe: the quiet first-run launcher of the player package (#3962). A small native Windows GUI
# program: it finds the player's Forces of Corruption install and starts the
# bundled Godot runtime with no console window. It stands alone on purpose: no EAWR library, the C++
# runtime linked statically and only system DLLs imported, so a clean PC needs no redistributable.
enable_language(RC)

# The Steam text formats and path helpers are shared with the mod core (src/mods/steam). EAWR.exe compiles
# those sources itself rather than linking eawr_mods: an OBJECT or STATIC library carries one C runtime choice,
# and the launcher needs the static one while every other EAWR target uses the DLL runtime.
include("${PROJECT_SOURCE_DIR}/src/mods/sources.cmake")

set(EAWR_LAUNCHER_CORE_SOURCES
    src/install_discovery.cpp
    src/launch_plan.cpp
    src/launcher_flow.cpp
    src/launcher_state.cpp
    src/win_process.cpp
    src/win_registry.cpp
    src/win_ui.cpp
    ${EAWR_MODS_STEAM_SOURCES}
)

# Static CRT: nothing from the Visual C++ redistributable is imported (tests/launcher_smoke.py checks).
set(EAWR_LAUNCHER_RUNTIME "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(eawr_launcher_core STATIC ${EAWR_LAUNCHER_CORE_SOURCES})
# Headers only from include/: the moved Steam parsers (include/eawr/mods/steam).
target_include_directories(eawr_launcher_core PUBLIC src "${PROJECT_SOURCE_DIR}/include")
target_compile_features(eawr_launcher_core PUBLIC cxx_std_20)
target_compile_definitions(eawr_launcher_core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(eawr_launcher_core PUBLIC advapi32 ole32 shell32 user32 uuid)
set_target_properties(eawr_launcher_core PROPERTIES MSVC_RUNTIME_LIBRARY "${EAWR_LAUNCHER_RUNTIME}")
eawr_target_project_warnings(eawr_launcher_core)

add_executable(eawr_launcher WIN32 src/win_main.cpp src/eawr_launcher.rc)
target_link_libraries(eawr_launcher PRIVATE eawr_launcher_core)
set_target_properties(eawr_launcher PROPERTIES OUTPUT_NAME EAWR MSVC_RUNTIME_LIBRARY "${EAWR_LAUNCHER_RUNTIME}")
set_source_files_properties(src/eawr_launcher.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/eawr_launcher.manifest")
if(MSVC)
    # The manifest (asInvoker, common controls 6, per-monitor DPI) is a resource of eawr_launcher.rc.
    target_link_options(eawr_launcher PRIVATE /MANIFEST:NO)
endif()
eawr_target_project_warnings(eawr_launcher)

# Portable manager entry point: GUI subsystem, static CRT, Windows system libraries only.
add_executable(eawr_mods_start WIN32 src/mods_start.cpp src/mods_start.rc)
target_link_libraries(eawr_mods_start PRIVATE eawr_launcher_core)
set_target_properties(eawr_mods_start PROPERTIES OUTPUT_NAME "EAWR Mods" MSVC_RUNTIME_LIBRARY "${EAWR_LAUNCHER_RUNTIME}")
set_source_files_properties(src/mods_start.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/eawr_launcher.manifest")
if(MSVC)
    target_link_options(eawr_mods_start PRIVATE /MANIFEST:NO)
endif()
eawr_target_project_warnings(eawr_mods_start)
