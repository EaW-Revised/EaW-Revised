# Standalone synthetic GDExtension that draws the WP-43 legacy/ family
# adapters through the production GodotRenderer and reports sampled pixels.
# It is not part of the root build (no godot-cpp there) or the viewer;
# configure it against the pinned godot-cpp cache exactly as apps/viewer does.
cmake_minimum_required(VERSION 3.25)

project(EawrLegacyFamilyProbe LANGUAGES CXX)

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

add_library(eawr_legacy_family_probe SHARED
    legacy_family_probe.cpp
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
target_include_directories(eawr_legacy_family_probe PRIVATE
    "${EAWR_ROOT}/include"
    "${EAWR_ROOT}/src/sim"
    "${EAWR_ROOT}/src/presentation/godot"
)
target_link_libraries(eawr_legacy_family_probe PRIVATE godot-cpp)
get_target_property(GODOTCPP_SUFFIX godot-cpp GODOTCPP_SUFFIX)
# Output stays in the build tree; the runtime test copies it into a temporary
# copy of project/, so the source tree never receives binaries or .godot data.
set_target_properties(eawr_legacy_family_probe PROPERTIES
    CXX_VISIBILITY_PRESET "${GODOTCPP_SYMBOL_VISIBILITY}"
    VISIBILITY_INLINES_HIDDEN ON
    PREFIX "lib"
    OUTPUT_NAME "eawr_legacy_family_probe${GODOTCPP_SUFFIX}"
    LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
    PDB_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
)

if(MSVC)
    target_compile_options(eawr_legacy_family_probe PRIVATE /W4 /WX /permissive- /EHsc)
else()
    target_compile_options(eawr_legacy_family_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
