cmake_minimum_required(VERSION 3.25)

project(EawrViewer VERSION 0.1.0 LANGUAGES CXX)
find_package(Threads REQUIRED)

set(EAWR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "EmpireAtWar-Revised source root")
set(EAWR_GODOT_CPP_ROOT "${EAWR_ROOT}/out/godot/deps/godot-cpp" CACHE PATH
    "Pinned godot-cpp 10.0.0-stable checkout (an external shared cache is supported)")
set(EAWR_GODOT_CPP_LIBRARY "" CACHE FILEPATH
    "Optional accepted prebuilt godot-cpp library; avoids recompiling the pinned binding cache")
set(EAWR_GODOT_CPP_GENERATED_INCLUDE "${EAWR_GODOT_CPP_ROOT}/gen/include" CACHE PATH
    "Generated include directory matching the accepted prebuilt godot-cpp library")
set(GODOTCPP_API_VERSION "4.7" CACHE STRING "Godot API version" FORCE)
set(GODOTCPP_TARGET "template_release" CACHE STRING "Godot target" FORCE)
set(GODOTCPP_USE_HOT_RELOAD OFF CACHE BOOL "Viewer does not use hot reload" FORCE)
if(NOT WIN32)
    # A source-built binding links libstdc++ dynamically, like the accepted prebuilt
    # binding. With godot-cpp's static default a Clang 18 build crashed (SIGSEGV)
    # in libstdc++ stream code under the official Godot 4.7.2 Linux binary.
    set(GODOTCPP_USE_STATIC_CPP OFF CACHE BOOL "Link libstdc++ dynamically" FORCE)
endif()
# The owner's reserve switch: no local configure or build (of any target, godot-cpp too) while it is on.
include("${EAWR_ROOT}/cmake/EawrReserveGuard.cmake")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT EXISTS "${EAWR_GODOT_CPP_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "Pinned godot-cpp not found at ${EAWR_GODOT_CPP_ROOT}; set EAWR_GODOT_CPP_ROOT to the accepted cache")
endif()

if(EAWR_GODOT_CPP_LIBRARY)
    if(NOT EXISTS "${EAWR_GODOT_CPP_LIBRARY}")
        message(FATAL_ERROR "EAWR_GODOT_CPP_LIBRARY does not exist: ${EAWR_GODOT_CPP_LIBRARY}")
    endif()
    add_library(godot-cpp STATIC IMPORTED GLOBAL)
    set_target_properties(godot-cpp PROPERTIES
        IMPORTED_LOCATION "${EAWR_GODOT_CPP_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES
            "${EAWR_GODOT_CPP_ROOT}/include;${EAWR_GODOT_CPP_GENERATED_INCLUDE}"
    )
    if(MSVC)
        target_compile_options(godot-cpp INTERFACE /utf-8 /Zc:__cplusplus)
        target_compile_definitions(godot-cpp INTERFACE
            WINDOWS_ENABLED TYPED_METHOD_BIND NOMINMAX GDEXTENSION _HAS_EXCEPTIONS=0 THREADS_ENABLED)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_compile_definitions(godot-cpp INTERFACE
            GDEXTENSION LINUX_ENABLED UNIX_ENABLED THREADS_ENABLED)
    endif()
    set(GODOTCPP_SYMBOL_VISIBILITY hidden)
    if(WIN32)
        set_property(TARGET godot-cpp PROPERTY GODOTCPP_SUFFIX ".windows.template_release.x86_64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        set_property(TARGET godot-cpp PROPERTY GODOTCPP_SUFFIX ".linux.template_release.arm64")
    else()
        set_property(TARGET godot-cpp PROPERTY GODOTCPP_SUFFIX ".linux.template_release.x86_64")
    endif()
else()
    add_subdirectory("${EAWR_GODOT_CPP_ROOT}" "${CMAKE_BINARY_DIR}/godot-cpp" EXCLUDE_FROM_ALL)
endif()

# The map mode resolves a TED skydome field against the XML catalog, so the
# viewer needs the same generated schema contract the root build generates.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${EAWR_ROOT}/data/skirmish/roster-gate.json"
    "${EAWR_ROOT}/tools/inventory/roster_gate.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${EAWR_ROOT}/tools/inventory/roster_gate.py"
    --header "${CMAKE_BINARY_DIR}/generated/roster_gate_data.hpp" COMMAND_ERROR_IS_FATAL ANY)
set(EAWR_VIEWER_XML_SCHEMA_INC "${CMAKE_BINARY_DIR}/generated/xml_schema_contract.inc")
add_custom_command(
    OUTPUT "${EAWR_VIEWER_XML_SCHEMA_INC}"
    COMMAND
        "${Python3_EXECUTABLE}" "${EAWR_ROOT}/src/data/generate_schema_contract.py"
        --input "${EAWR_ROOT}/plan/inventories/xml-tags.json"
        --output "${EAWR_VIEWER_XML_SCHEMA_INC}"
    DEPENDS
        "${EAWR_ROOT}/src/data/generate_schema_contract.py"
        "${EAWR_ROOT}/plan/inventories/xml-tags.json"
    VERBATIM
)

# #79: the FoC tactical AI of the live battle: the authoritative Lua runtime (the soft-float
# profile, upstream Lua 5.0.2 compiled as C++ in its own namespace, the sandbox and the
# scheduler) and the freestore host. Its own library: upstream Lua keeps its own warnings and
# private include paths, as in the root build (src/script/CMakeLists.txt).
include("${EAWR_ROOT}/src/core/sources.cmake")
include("${EAWR_ROOT}/src/sim/sources.cmake")
include("${EAWR_ROOT}/src/vfs/sources.cmake")
include("${EAWR_ROOT}/src/data/sources.cmake")
include("${EAWR_ROOT}/src/assets/sources.cmake")
include("${EAWR_ROOT}/src/data/ui/sources.cmake")
include("${EAWR_ROOT}/src/presentation/sources.cmake")
include("${EAWR_ROOT}/src/presentation/ui/sources.cmake")
include("${EAWR_ROOT}/src/scene/sources.cmake")
include("${EAWR_ROOT}/src/units/sources.cmake")
include("${EAWR_ROOT}/src/skirmish/sources.cmake")
include("${EAWR_ROOT}/src/platform/sources.cmake")
include("${EAWR_ROOT}/src/script/sources.cmake")

add_library(eawr_viewer_script STATIC
    ${EAWR_SCRIPT_NUMERIC_SOURCES}
    ${EAWR_SFLUA_HOOKS_SOURCES}
    ${EAWR_SFLUA_SOURCES}
    ${EAWR_SCRIPT_AUTHORITATIVE_SOURCES}
    ${EAWR_SCRIPT_FOC_SOURCES}
)
target_include_directories(eawr_viewer_script PUBLIC "${EAWR_ROOT}/include")
target_include_directories(eawr_viewer_script PRIVATE "${EAWR_ROOT}/src/script/sflua" "${EAWR_ROOT}/src/script/authoritative")
target_include_directories(eawr_viewer_script SYSTEM PRIVATE
    "${EAWR_ROOT}/third_party/lua/include"
    "${EAWR_ROOT}/third_party/lua/src"
    "${EAWR_ROOT}/third_party/lua/src/lib"
)
target_compile_features(eawr_viewer_script PUBLIC cxx_std_20)
if(NOT MSVC)
    target_compile_options(eawr_viewer_script PRIVATE -fno-strict-aliasing)
endif()

add_library(eawr_viewer_core STATIC
    ${EAWR_CORE_SOURCES}
    ${EAWR_SIM_SOURCES}
    ${EAWR_VFS_SOURCES}
    ${EAWR_DATA_SOURCES}
    ${EAWR_ASSETS_SOURCES}
    ${EAWR_UI_DATA_SOURCES}
    ${EAWR_PRESENTATION_SOURCES}
    ${EAWR_UI_MODEL_SOURCES}
    ${EAWR_SCENE_SOURCES}
    ${EAWR_UNITS_SOURCES}
    ${EAWR_SKIRMISH_SOURCES}
    ${EAWR_PLATFORM_SOURCES}
    ${EAWR_PLATFORM_LIVE_AI_SOURCES}
    "${EAWR_ROOT}/third_party/pugixml/src/pugixml.cpp"
    "${EAWR_VIEWER_XML_SCHEMA_INC}"
)
target_include_directories(eawr_viewer_core PUBLIC
    "${EAWR_ROOT}/include"
    "${EAWR_ROOT}/third_party/pugixml/src"
    "${EAWR_ROOT}/third_party/entt/single_include"
)
# src/sim/fog.cpp includes the private sim headers beside it; src/data/ui/command_bar.cpp
# includes src/data/xml_internal.hpp.
target_include_directories(eawr_viewer_core PRIVATE "${CMAKE_BINARY_DIR}/generated" "${EAWR_ROOT}/src/sim" "${EAWR_ROOT}/src/data")
# The script library and the core use each other (the AI host reads the simulation; the live
# session hosts the scripts); CMake repeats the two static libraries on the link line.
target_link_libraries(eawr_viewer_core PUBLIC eawr_viewer_script)
target_link_libraries(eawr_viewer_script PUBLIC eawr_viewer_core)

add_library(eawr_viewer SHARED
    src/register_types.cpp
    src/viewer_host.cpp
    src/viewer_host_ready.cpp
    src/viewer_host_config.cpp
    src/viewer_host_scene.cpp
    src/viewer_host_process.cpp
    src/viewer_host_camera.cpp
    src/viewer_host_camera_setup.cpp
    src/viewer_host_camera_probe.cpp
    src/viewer_host_camera_steps.cpp
    src/viewer_host_exercises.cpp
    src/viewer_host_exercises_renderer.cpp
    src/viewer_host_exercises_effects.cpp
    src/viewer_host_report.cpp
    src/viewer_host_report_scene.cpp
    src/model_preview.cpp
    src/camera_binding_config.cpp
    src/camera_binding_tokens.cpp
    src/camera_binding_values.cpp
    src/camera_binding_table.cpp
    src/camera_input.cpp
    src/map_camera.cpp
    src/map_mode.cpp
    src/map_mode_ready.cpp
    src/map_mode_ready_terrain.cpp
    src/map_mode_ready_population.cpp
    src/map_mode_live.cpp
    src/map_mode_camera_probe.cpp
    src/map_mode_particles.cpp
    src/map_mode_hud.cpp
    src/map_mode_report.cpp
    src/map_mode_report_scene.cpp
    src/map_mode_report_effects.cpp
    src/map_mode_report_live.cpp
    src/map_mode_scene.cpp
    src/map_mode_placements.cpp
    src/map_mode_units.cpp
    src/map_mode_assets.cpp
    src/land_look.cpp
    src/fog_mode.cpp
    src/space_environment.cpp
    src/space_environment_config.cpp
    src/space_environment_prepare.cpp
    src/space_environment_probe.cpp
    src/space_environment_fog.cpp
    src/space_environment_report.cpp
    src/space_environment_report_surfaces.cpp
    src/space_environment_view.cpp
    src/space_environment_surfaces.cpp
    src/space_environment_effects.cpp
    src/space_fog.cpp
    src/space_fog_units.cpp
    src/space_fog_units_compose.cpp
    src/space_populate.cpp
    src/space_populate_compose.cpp
    src/space_populate_materials.cpp
    src/space_populate_animation.cpp
    src/space_populate_report.cpp
    src/space_populate_assets.cpp
    src/battle_audio.cpp
    src/battle_audio_prepare.cpp
    src/battle_audio_events.cpp
    src/battle_audio_report.cpp
    src/battle_input.cpp
    src/battle_input_cards.cpp
    src/battle_input_commands.cpp
    src/battle_input_pick.cpp
    src/battle_input_production.cpp
    src/battle_input_replay.cpp
    src/battle_input_report.cpp
    src/battle_cursor.cpp
    src/world_ui_view.cpp
    src/world_ui_prepare.cpp
    src/world_ui_groups.cpp
    src/world_ui_bars.cpp
    src/world_ui_report.cpp
    src/overview_fade_view.cpp
    src/battle_effects.cpp
    src/battle_effects_prepare.cpp
    src/battle_effects_projectiles.cpp
    src/battle_effects_impacts.cpp
    src/battle_effects_report.cpp
    src/unit_emitters.cpp
    src/unit_emitters_prepare.cpp
    src/unit_emitters_frame.cpp
    src/unit_emitters_report.cpp
    src/perf_trace.cpp
    src/debris_props.cpp
    src/particle_workers.cpp
    src/live_fog_view.cpp
    src/live_session_view.cpp
    src/battle_scoring.cpp
    src/live_session_death.cpp
    src/live_session_economy.cpp
    src/live_session_fighters.cpp
    src/live_session_frame.cpp
    src/live_session_config.cpp
    src/live_session_prepare.cpp
    src/live_session_report.cpp
    src/effect_mode.cpp
    src/effect_mode_probe.cpp
    src/effect_mode_report.cpp
    src/effect_mode_report_probe.cpp
    src/unit_mode.cpp
    src/font_mode.cpp
    src/input_routing_mode.cpp
    src/ui_gallery_mode.cpp
    src/ui_gallery_controls.cpp
    src/ui_gallery_report.cpp
    src/skirmish_setup_mode.cpp
    "${EAWR_ROOT}/src/presentation/godot/renderer.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_fog.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_instances.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_upload.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_uniforms.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_materials.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_mesh_upload.cpp"
    "${EAWR_ROOT}/src/presentation/godot/fog_adapter.cpp"
    "${EAWR_ROOT}/src/presentation/godot/particle_adapter.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/font_provider.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/input_routing.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/kit.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/kit_text.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/kit_frames.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/kit_buttons.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/kit_controls.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/theme_builder.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/tactical_hud.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/tactical_hud_shell.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/tactical_hud_build.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/tactical_hud_resources.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/tactical_hud_report.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/battle_overlay.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/production_view.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/perf_overlay_view.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/unit_cards_view.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/minimap_view.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/ability_buttons_view.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/dialog_builder.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/movie_player.cpp"
)
target_include_directories(eawr_viewer PRIVATE
    src
    "${EAWR_ROOT}/include"
    "${EAWR_ROOT}/src/presentation/godot"
)
target_link_libraries(eawr_viewer PRIVATE godot-cpp eawr_viewer_core Threads::Threads)
get_target_property(GODOTCPP_SUFFIX godot-cpp GODOTCPP_SUFFIX)
set(PROJECT_BIN "${CMAKE_CURRENT_LIST_DIR}/project/bin")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/project/common")
configure_file("${EAWR_ROOT}/prototypes/common/scene.json"
    "${CMAKE_CURRENT_LIST_DIR}/project/common/scene.json" COPYONLY)
configure_file("${EAWR_ROOT}/tests/replay/fixtures/original-v1.eawr-replay"
    "${CMAKE_CURRENT_LIST_DIR}/project/common/original-v1.eawr-replay" COPYONLY)
set_target_properties(eawr_viewer PROPERTIES
    CXX_VISIBILITY_PRESET "${GODOTCPP_SYMBOL_VISIBILITY}"
    VISIBILITY_INLINES_HIDDEN ON
    PREFIX "lib"
    OUTPUT_NAME "eawr_viewer${GODOTCPP_SUFFIX}"
    LIBRARY_OUTPUT_DIRECTORY "$<1:${PROJECT_BIN}>"
    RUNTIME_OUTPUT_DIRECTORY "$<1:${PROJECT_BIN}>"
    PDB_OUTPUT_DIRECTORY "$<1:${PROJECT_BIN}>"
)

if(MSVC)
    target_compile_options(eawr_viewer_core PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(eawr_viewer PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(eawr_viewer_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(eawr_viewer PRIVATE -Wall -Wextra -Wpedantic)
endif()
