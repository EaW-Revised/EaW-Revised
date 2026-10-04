# Standalone synthetic GDExtension that drives the production GodotRenderer
# through its shader-variant, fog-variant, upload and binding failure branches
# (#22, WP-08). Like the churn probe it is not part of the root build and is
# configured against the pinned godot-cpp cache. renderer.cpp is compiled
# unchanged, with adapter_source_faults.hpp force-included into that one
# translation unit only (see that header); this library is test evidence, not
# a production binary.
cmake_minimum_required(VERSION 3.25)

project(EawrRendererFaultProbe LANGUAGES CXX)

get_filename_component(EAWR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
include("${EAWR_ROOT}/cmake/EawrProbeSources.cmake")

add_library(eawr_renderer_fault_probe SHARED
    renderer_fault_fog_tests.cpp
    renderer_fault_material_tests.cpp
    renderer_fault_probe.cpp
    "${EAWR_ROOT}/src/presentation/godot/renderer.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_fog.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_instances.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_upload.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_uniforms.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_materials.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_mesh_upload.cpp"
    "${EAWR_ROOT}/src/presentation/godot/fog_adapter.cpp"
)
target_include_directories(eawr_renderer_fault_probe PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${EAWR_ROOT}/include"
    "${EAWR_ROOT}/src/sim"
    "${EAWR_ROOT}/src/presentation/godot"
)
target_link_libraries(eawr_renderer_fault_probe PRIVATE godot-cpp eawr_viewer_core)
get_target_property(GODOTCPP_SUFFIX godot-cpp GODOTCPP_SUFFIX)
# Output stays in the build tree; the runtime test copies it into a temporary
# copy of project/, so the source tree never receives binaries or .godot data.
set_target_properties(eawr_renderer_fault_probe PROPERTIES
    CXX_VISIBILITY_PRESET "${GODOTCPP_SYMBOL_VISIBILITY}"
    VISIBILITY_INLINES_HIDDEN ON
    PREFIX "lib"
    OUTPUT_NAME "eawr_renderer_fault_probe${GODOTCPP_SUFFIX}"
    LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
    PDB_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
)

if(MSVC)
    target_compile_options(eawr_renderer_fault_probe PRIVATE /W4 /WX /permissive- /EHsc)
else()
    target_compile_options(eawr_renderer_fault_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# The fault seam reaches the renderer implementation files only: their fixed
# adapter-source names are redirected to fault_injection::pick() before the
# files' own includes run.
if(MSVC)
    set(EAWR_FAULT_FORCE_INCLUDE "/FI${CMAKE_CURRENT_LIST_DIR}/adapter_source_faults.hpp")
else()
    set(EAWR_FAULT_FORCE_INCLUDE "-include;${CMAKE_CURRENT_LIST_DIR}/adapter_source_faults.hpp")
endif()
set_source_files_properties(
    "${EAWR_ROOT}/src/presentation/godot/renderer.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_fog.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_instances.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_upload.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_uniforms.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_materials.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_mesh_upload.cpp"
    PROPERTIES COMPILE_OPTIONS "${EAWR_FAULT_FORCE_INCLUDE}")
