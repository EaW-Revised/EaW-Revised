cmake_minimum_required(VERSION 3.25)
project(EawrShaderBridgeGpu LANGUAGES CXX)
option(EAWR_ENABLE_SHADER_BRIDGE_GATE "Build the opt-in shader experiment" OFF)
if(NOT EAWR_ENABLE_SHADER_BRIDGE_GATE)
    return()
endif()
get_filename_component(EAWR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
include("${EAWR_ROOT}/cmake/EawrProbeSources.cmake")
add_library(eawr_shader_bridge_probe SHARED probe.cpp
    "${EAWR_ROOT}/src/presentation/godot/renderer.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_fog.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_instances.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_upload.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_uniforms.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_materials.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_mesh_upload.cpp"
    "${EAWR_ROOT}/src/presentation/godot/fog_adapter.cpp")
target_include_directories(eawr_shader_bridge_probe PRIVATE "${EAWR_ROOT}/include"
    "${EAWR_ROOT}/src/sim" "${EAWR_ROOT}/src/presentation/godot")
target_link_libraries(eawr_shader_bridge_probe PRIVATE godot-cpp eawr_viewer_core)
set_target_properties(eawr_shader_bridge_probe PROPERTIES PREFIX "" OUTPUT_NAME "eawr_shader_bridge_probe"
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>")
if(MSVC)
    target_compile_options(eawr_shader_bridge_probe PRIVATE /W4 /WX /permissive- /EHsc)
else()
    target_compile_options(eawr_shader_bridge_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
