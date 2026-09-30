cmake_minimum_required(VERSION 3.25)

project(EawrViewer VERSION 0.1.0 LANGUAGES CXX)

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
set(_eawr_viewer_sflua_units
    lapi lcode ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate
    lstring ltable ltm lundump lvm lzio lauxlib lbaselib lstrlib ltablib)
set(_eawr_viewer_script_sources
    "${EAWR_ROOT}/src/script/numeric/binary64.cpp"
    "${EAWR_ROOT}/src/script/numeric/decimal.cpp"
    "${EAWR_ROOT}/src/script/numeric/q24_boundary.cpp"
    "${EAWR_ROOT}/src/script/sflua/sflua_hooks.cpp"
    "${EAWR_ROOT}/src/script/sflua/sflua_state.cpp"
    "${EAWR_ROOT}/src/script/sflua/sflua_sandbox.cpp"
    "${EAWR_ROOT}/src/script/sflua/sflua_persist.cpp"
    "${EAWR_ROOT}/src/script/authoritative/bindings.cpp"
    "${EAWR_ROOT}/src/script/authoritative/clock_rng.cpp"
    "${EAWR_ROOT}/src/script/authoritative/persistence.cpp"
    "${EAWR_ROOT}/src/script/authoritative/scheduler.cpp"
    "${EAWR_ROOT}/src/script/authoritative/tactical_bridge.cpp"
    "${EAWR_ROOT}/src/script/foc/tactical_ai.cpp"
    "${EAWR_ROOT}/src/script/foc/ai_equations.cpp"
    "${EAWR_ROOT}/src/script/foc/ai_data.cpp"
    "${EAWR_ROOT}/src/script/foc/ai_perception.cpp"
    "${EAWR_ROOT}/src/script/foc/ai_engine.cpp"
    "${EAWR_ROOT}/src/script/foc/ai_plans.cpp"
    "${EAWR_ROOT}/src/script/foc/plan_bindings.cpp"
)
foreach(_eawr_unit IN LISTS _eawr_viewer_sflua_units)
    list(APPEND _eawr_viewer_script_sources "${EAWR_ROOT}/src/script/sflua/upstream/${_eawr_unit}.cpp")
endforeach()
add_library(eawr_viewer_script STATIC ${_eawr_viewer_script_sources})
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
    "${EAWR_ROOT}/src/core/diagnostic.cpp"
    "${EAWR_ROOT}/src/core/sha256.cpp"
    "${EAWR_ROOT}/src/vfs/vfs.cpp"
    "${EAWR_ROOT}/src/vfs/vfs_archive.cpp"
    "${EAWR_ROOT}/third_party/pugixml/src/pugixml.cpp"
    "${EAWR_ROOT}/src/assets/animation.cpp"
    "${EAWR_ROOT}/src/presentation/animation/animation.cpp"
    "${EAWR_ROOT}/src/presentation/animation/idle_playback.cpp"
    "${EAWR_ROOT}/src/presentation/animation/unit_clips.cpp"
    "${EAWR_ROOT}/src/presentation/audio/sfx.cpp"
    "${EAWR_ROOT}/src/presentation/particles/alo_particles.cpp"
    "${EAWR_ROOT}/src/presentation/particles/map_effect_plan.cpp"
    "${EAWR_ROOT}/src/presentation/particles/cpu_system.cpp"
    "${EAWR_ROOT}/src/presentation/particles/proxy_binding.cpp"
    "${EAWR_ROOT}/src/presentation/particles/render.cpp"
    "${EAWR_ROOT}/src/assets/map.cpp"
    "${EAWR_ROOT}/src/assets/map_decode.cpp"
    "${EAWR_ROOT}/src/assets/map_references.cpp"
    "${EAWR_ROOT}/src/assets/model.cpp"
    "${EAWR_ROOT}/src/assets/texture.cpp"
    "${EAWR_ROOT}/src/assets/mega_texture.cpp"
    "${EAWR_ROOT}/src/data/tag_trace.cpp"
    "${EAWR_ROOT}/src/data/xml.cpp"
    "${EAWR_ROOT}/src/data/xml_merge.cpp"
    "${EAWR_ROOT}/src/data/xml_override.cpp"
    "${EAWR_ROOT}/src/data/xml_registry.cpp"
    "${EAWR_VIEWER_XML_SCHEMA_INC}"
    "${EAWR_ROOT}/src/presentation/camera/camera.cpp"
    "${EAWR_ROOT}/src/presentation/camera/constants_source.cpp"
    "${EAWR_ROOT}/src/presentation/camera/controller.cpp"
    "${EAWR_ROOT}/src/presentation/camera/free_camera.cpp"
    "${EAWR_ROOT}/src/presentation/camera/input.cpp"
    "${EAWR_ROOT}/src/presentation/camera/overview.cpp"
    "${EAWR_ROOT}/src/presentation/fog/fog.cpp"
    "${EAWR_ROOT}/src/presentation/lighting/lighting.cpp"
    "${EAWR_ROOT}/src/presentation/lighting/scene_bloom.cpp"
    "${EAWR_ROOT}/src/presentation/lighting/wind.cpp"
    "${EAWR_ROOT}/src/presentation/terrain/terrain.cpp"
    "${EAWR_ROOT}/src/presentation/space/environment_scene.cpp"
    "${EAWR_ROOT}/src/presentation/space/space.cpp"
    "${EAWR_ROOT}/src/presentation/space/space_effects.cpp"
    "${EAWR_ROOT}/src/presentation/space/space_geometry.cpp"
    "${EAWR_ROOT}/src/presentation/space/space_surfaces.cpp"
    "${EAWR_ROOT}/src/presentation/space/sun_retail.cpp"
    "${EAWR_ROOT}/src/data/ui/command_bar.cpp"
    "${EAWR_ROOT}/src/data/ui/sfnt.cpp"
    "${EAWR_ROOT}/src/data/ui/shell_anchors.cpp"
    "${EAWR_ROOT}/src/data/ui/dialog_catalog.cpp"
    "${EAWR_ROOT}/src/data/ui/dialog_script.cpp"
    "${EAWR_ROOT}/src/data/ui/text_database.cpp"
    "${EAWR_ROOT}/src/data/ui/movie.cpp"
    "${EAWR_ROOT}/src/presentation/ui/command_sink.cpp"
    "${EAWR_ROOT}/src/presentation/ui/fonts.cpp"
    "${EAWR_ROOT}/src/presentation/ui/hud.cpp"
    "${EAWR_ROOT}/src/presentation/ui/hud_shell.cpp"
    "${EAWR_ROOT}/src/presentation/ui/input_routing.cpp"
    "${EAWR_ROOT}/src/presentation/ui/layout.cpp"
    "${EAWR_ROOT}/src/presentation/ui/minimap.cpp"
    "${EAWR_ROOT}/src/presentation/ui/perf_stats.cpp"
    "${EAWR_ROOT}/src/presentation/ui/selection.cpp"
    "${EAWR_ROOT}/src/presentation/ui/world_ui.cpp"
    "${EAWR_ROOT}/src/presentation/ui/time_controls.cpp"
    "${EAWR_ROOT}/src/presentation/ui/ability_buttons.cpp"
    "${EAWR_ROOT}/src/presentation/ui/battle_messages.cpp"
    "${EAWR_ROOT}/src/presentation/ui/shell_alpha.cpp"
    "${EAWR_ROOT}/src/presentation/ui/theme.cpp"
    "${EAWR_ROOT}/src/presentation/ui/unit_cards.cpp"
    "${EAWR_ROOT}/src/scene/idle_tags.cpp"
    "${EAWR_ROOT}/src/scene/scene.cpp"
    "${EAWR_ROOT}/src/scene/scene_assets.cpp"
    "${EAWR_ROOT}/src/scene/scene_build.cpp"
    "${EAWR_ROOT}/src/scene/scene_evidence.cpp"
    "${EAWR_ROOT}/src/scene/space_population.cpp"
    "${EAWR_ROOT}/src/sim/fog.cpp"
    "${EAWR_ROOT}/src/sim/replay.cpp"
    "${EAWR_ROOT}/src/sim/snapshot.cpp"
    "${EAWR_ROOT}/src/sim/world.cpp"
    "${EAWR_ROOT}/src/sim/math/fixed.cpp"
    "${EAWR_ROOT}/src/sim/math/geometry.cpp"
    "${EAWR_ROOT}/src/sim/math/trig.cpp"
    "${EAWR_ROOT}/src/sim/math/wide.cpp"
    # #80: the live tactical session (M2 skirmish start, unit tables, the
    # simulation thread and its worker pool) and its presentation.
    "${EAWR_ROOT}/src/sim/tactical/abilities.cpp"
    "${EAWR_ROOT}/src/sim/tactical/collection.cpp"
    "${EAWR_ROOT}/src/sim/tactical/collision.cpp"
    "${EAWR_ROOT}/src/sim/tactical/combat.cpp"
    "${EAWR_ROOT}/src/sim/tactical/damage.cpp"
    "${EAWR_ROOT}/src/sim/tactical/projectiles.cpp"
    "${EAWR_ROOT}/src/sim/tactical/durability.cpp"
    "${EAWR_ROOT}/src/sim/tactical/encoding.cpp"
    "${EAWR_ROOT}/src/sim/tactical/fog_cells.cpp"
    "${EAWR_ROOT}/src/sim/tactical/ion.cpp"
    "${EAWR_ROOT}/src/sim/tactical/motion.cpp"
    "${EAWR_ROOT}/src/sim/tactical/fighters.cpp"
    "${EAWR_ROOT}/src/sim/tactical/death_spin.cpp"
    "${EAWR_ROOT}/src/sim/tactical/formation.cpp"
    "${EAWR_ROOT}/src/sim/tactical/orders.cpp"
    "${EAWR_ROOT}/src/sim/tactical/pathfind.cpp"
    "${EAWR_ROOT}/src/sim/tactical/replay.cpp"
    "${EAWR_ROOT}/src/sim/tactical/session.cpp"
    "${EAWR_ROOT}/src/sim/tactical/space.cpp"
    "${EAWR_ROOT}/src/sim/tactical/victory.cpp"
    "${EAWR_ROOT}/src/sim/tactical/visibility.cpp"
    "${EAWR_ROOT}/src/units/unit_abilities.cpp"
    "${EAWR_ROOT}/src/units/unit_combat.cpp"
    "${EAWR_ROOT}/src/units/unit_durability.cpp"
    "${EAWR_ROOT}/src/units/unit_identity.cpp"
    "${EAWR_ROOT}/src/units/unit_motion.cpp"
    "${EAWR_ROOT}/src/units/unit_priority.cpp"
    "${EAWR_ROOT}/src/units/unit_support.cpp"
    "${EAWR_ROOT}/src/units/unit_tables.cpp"
    "${EAWR_ROOT}/src/skirmish/ai.cpp"
    "${EAWR_ROOT}/src/skirmish/census.cpp"
    "${EAWR_ROOT}/src/skirmish/content.cpp"
    "${EAWR_ROOT}/src/skirmish/inputs.cpp"
    "${EAWR_ROOT}/src/skirmish/melee.cpp"
    "${EAWR_ROOT}/src/platform/live_ai.cpp"
    "${EAWR_ROOT}/src/skirmish/placement.cpp"
    "${EAWR_ROOT}/src/skirmish/start.cpp"
    "${EAWR_ROOT}/src/platform/live_session.cpp"
    "${EAWR_ROOT}/src/platform/sim_workers.cpp"
    "${EAWR_ROOT}/src/presentation/space/debris.cpp"
    "${EAWR_ROOT}/src/presentation/space/fog_field.cpp"
    "${EAWR_ROOT}/src/presentation/space/live_units.cpp"
    "${EAWR_ROOT}/src/presentation/space/projectiles.cpp"
    "${EAWR_ROOT}/src/presentation/space/shield_hits.cpp"
    "${EAWR_ROOT}/src/presentation/space/unit_fade.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_contract.cpp"
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
    src/viewer_host_camera.cpp
    src/viewer_host_exercises.cpp
    src/viewer_host_report.cpp
    src/model_preview.cpp
    src/camera_binding_config.cpp
    src/camera_input.cpp
    src/map_camera.cpp
    src/map_mode.cpp
    src/map_mode_particles.cpp
    src/map_mode_hud.cpp
    src/map_mode_report.cpp
    src/map_mode_scene.cpp
    src/land_look.cpp
    src/fog_mode.cpp
    src/space_environment.cpp
    src/space_environment_fog.cpp
    src/space_environment_report.cpp
    src/space_environment_view.cpp
    src/space_fog.cpp
    src/space_fog_units.cpp
    src/space_populate.cpp
    src/space_populate_assets.cpp
    src/battle_audio.cpp
    src/battle_input.cpp
    src/world_ui_view.cpp
    src/battle_effects.cpp
    src/unit_emitters.cpp
    src/perf_trace.cpp
    src/debris_props.cpp
    src/live_fog_view.cpp
    src/live_session_view.cpp
    src/effect_mode.cpp
    src/effect_mode_probe.cpp
    src/effect_mode_report.cpp
    src/unit_mode.cpp
    src/font_mode.cpp
    src/input_routing_mode.cpp
    src/ui_gallery_mode.cpp
    "${EAWR_ROOT}/src/presentation/godot/renderer.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_fog.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_instances.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_upload.cpp"
    "${EAWR_ROOT}/src/presentation/godot/fog_adapter.cpp"
    "${EAWR_ROOT}/src/presentation/godot/particle_adapter.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/font_provider.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/input_routing.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/kit.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/theme_builder.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/tactical_hud.cpp"
    "${EAWR_ROOT}/src/presentation/godot/ui/battle_overlay.cpp"
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
target_link_libraries(eawr_viewer PRIVATE godot-cpp eawr_viewer_core)
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
