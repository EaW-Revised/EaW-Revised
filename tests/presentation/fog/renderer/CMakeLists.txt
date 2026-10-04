# Standalone synthetic GDExtension that drives the production GodotRenderer's
# fog-stub-v1 binding (P1-07, #28). It is not part of the root build (no
# godot-cpp there) or the viewer; configure it against the pinned godot-cpp
# cache exactly as apps/viewer does.
cmake_minimum_required(VERSION 3.25)

project(EawrFogRendererProbe LANGUAGES CXX)

get_filename_component(EAWR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
include("${EAWR_ROOT}/cmake/EawrProbeSources.cmake")

add_library(eawr_fog_renderer_probe SHARED
    fog_renderer_evidence.cpp
    fog_renderer_verify.cpp
    fog_renderer_instance_probe.cpp
    fog_renderer_instance_steps.cpp
    fog_renderer_alpha_steps.cpp
    fog_renderer_probe.cpp
    fog_renderer_surface_probe.cpp
    "${EAWR_ROOT}/src/presentation/godot/renderer.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_fog.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_instances.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_upload.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_uniforms.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_materials.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_mesh_upload.cpp"
    "${EAWR_ROOT}/src/presentation/godot/fog_adapter.cpp"
)
target_include_directories(eawr_fog_renderer_probe PRIVATE
    "${EAWR_ROOT}/include"
    "${EAWR_ROOT}/src/sim"
    "${EAWR_ROOT}/src/presentation/godot"
)
target_link_libraries(eawr_fog_renderer_probe PRIVATE godot-cpp eawr_viewer_core)
get_target_property(GODOTCPP_SUFFIX godot-cpp GODOTCPP_SUFFIX)
# Output stays in the build tree; the runtime test copies it into a temporary
# copy of project/, so the source tree never receives binaries or .godot data.
set_target_properties(eawr_fog_renderer_probe PROPERTIES
    CXX_VISIBILITY_PRESET "${GODOTCPP_SYMBOL_VISIBILITY}"
    VISIBILITY_INLINES_HIDDEN ON
    PREFIX "lib"
    OUTPUT_NAME "eawr_fog_renderer_probe${GODOTCPP_SUFFIX}"
    LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
    PDB_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
)

if(MSVC)
    target_compile_options(eawr_fog_renderer_probe PRIVATE /W4 /WX /permissive- /EHsc)
else()
    target_compile_options(eawr_fog_renderer_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
