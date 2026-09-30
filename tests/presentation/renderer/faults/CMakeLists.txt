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
set(EAWR_GODOT_CPP_ROOT "${EAWR_ROOT}/out/godot/deps/godot-cpp" CACHE PATH
    "Pinned godot-cpp 10.0.0-stable checkout (an external shared cache is supported)")
set(EAWR_GODOT_CPP_LIBRARY "" CACHE FILEPATH
    "Optional accepted prebuilt godot-cpp library; avoids recompiling the pinned binding cache")
set(EAWR_GODOT_CPP_GENERATED_INCLUDE "${EAWR_GODOT_CPP_ROOT}/gen/include" CACHE PATH
    "Generated include directory matching the accepted prebuilt godot-cpp library")
set(GODOTCPP_API_VERSION "4.7" CACHE STRING "Godot API version" FORCE)
set(GODOTCPP_TARGET "template_release" CACHE STRING "Godot target" FORCE)
set(GODOTCPP_USE_HOT_RELOAD OFF CACHE BOOL "Probe does not use hot reload" FORCE)
# godot-cpp is third-party code: its headers are included as SYSTEM so the
# probe's -Werror applies to project code only. An imported prebuilt library
# gets that from CMake already; the in-tree build defaults it OFF, and GCC
# then fails on godot-cpp's own unused parameters before reaching ours.
set(GODOTCPP_SYSTEM_HEADERS ON CACHE BOOL "Expose godot-cpp headers as SYSTEM" FORCE)
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

add_library(eawr_renderer_fault_probe SHARED
    renderer_fault_fog_tests.cpp
    renderer_fault_material_tests.cpp
    renderer_fault_probe.cpp
    "${EAWR_ROOT}/src/presentation/godot/renderer.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_contract.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_fog.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_instances.cpp"
    "${EAWR_ROOT}/src/presentation/godot/renderer_upload.cpp"
    "${EAWR_ROOT}/src/presentation/godot/fog_adapter.cpp"
    "${EAWR_ROOT}/src/presentation/fog/fog.cpp"
    "${EAWR_ROOT}/src/presentation/animation/animation.cpp"
    "${EAWR_ROOT}/src/presentation/space/environment_scene.cpp"
    "${EAWR_ROOT}/src/presentation/space/sun_retail.cpp"
    "${EAWR_ROOT}/src/core/diagnostic.cpp"
    "${EAWR_ROOT}/src/core/sha256.cpp"
    "${EAWR_ROOT}/src/sim/fog.cpp"
    "${EAWR_ROOT}/src/sim/replay.cpp"
    "${EAWR_ROOT}/src/sim/snapshot.cpp"
    "${EAWR_ROOT}/src/sim/math/fixed.cpp"
    "${EAWR_ROOT}/src/sim/math/geometry.cpp"
    "${EAWR_ROOT}/src/sim/math/trig.cpp"
    "${EAWR_ROOT}/src/sim/math/wide.cpp"
)
target_include_directories(eawr_renderer_fault_probe PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${EAWR_ROOT}/include"
    "${EAWR_ROOT}/src/sim"
    "${EAWR_ROOT}/src/presentation/godot"
)
target_link_libraries(eawr_renderer_fault_probe PRIVATE godot-cpp)
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
    PROPERTIES COMPILE_OPTIONS "${EAWR_FAULT_FORCE_INCLUDE}")
