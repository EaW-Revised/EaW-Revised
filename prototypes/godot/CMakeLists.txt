cmake_minimum_required(VERSION 3.25)

project(EawrGodotPrototype VERSION 0.1.0 LANGUAGES CXX)

set(EAWR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "EmpireAtWar-Revised source root")
set(EAWR_GODOT_CPP_ROOT "${EAWR_ROOT}/out/godot/deps/godot-cpp" CACHE PATH "Pinned godot-cpp checkout")
set(EAWR_GODOT_CPP_LIBRARY "" CACHE FILEPATH
    "Optional accepted prebuilt godot-cpp library for reproducible P0 recapture")
set(EAWR_GODOT_CPP_GENERATED_INCLUDE "${EAWR_GODOT_CPP_ROOT}/gen/include" CACHE PATH
    "Generated include directory matching the accepted prebuilt godot-cpp library")
set(GODOTCPP_API_VERSION "4.7" CACHE STRING "Godot API version" FORCE)
set(GODOTCPP_TARGET "template_release" CACHE STRING "Godot target" FORCE)
set(GODOTCPP_USE_HOT_RELOAD OFF CACHE BOOL "Prototype does not use hot reload" FORCE)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT EXISTS "${EAWR_GODOT_CPP_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "Pinned godot-cpp not found at ${EAWR_GODOT_CPP_ROOT}; run install_dependencies.ps1")
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

add_library(eawr_godot_core STATIC
    "${EAWR_ROOT}/src/core/diagnostic.cpp"
    "${EAWR_ROOT}/src/vfs/vfs.cpp"
    "${EAWR_ROOT}/src/vfs/vfs_archive.cpp"
    "${EAWR_ROOT}/third_party/pugixml/src/pugixml.cpp"
    "${EAWR_ROOT}/src/assets/animation.cpp"
    "${EAWR_ROOT}/src/assets/model.cpp"
    "${EAWR_ROOT}/src/assets/texture.cpp"
    "${EAWR_ROOT}/src/sim/replay.cpp"
    "${EAWR_ROOT}/src/sim/snapshot.cpp"
    "${EAWR_ROOT}/src/sim/world.cpp"
    "${EAWR_ROOT}/src/sim/math/fixed.cpp"
    "${EAWR_ROOT}/src/sim/math/geometry.cpp"
    "${EAWR_ROOT}/src/sim/math/trig.cpp"
    "${EAWR_ROOT}/src/sim/math/wide.cpp"
)
target_include_directories(eawr_godot_core PUBLIC
    "${EAWR_ROOT}/include"
    "${EAWR_ROOT}/third_party/pugixml/src"
    "${EAWR_ROOT}/third_party/entt/single_include"
)
if(MSVC)
    target_compile_options(eawr_godot_core PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(eawr_godot_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(eawr_godot SHARED
    src/prototype_host.cpp
    src/register_types.cpp
)
target_include_directories(eawr_godot PRIVATE src "${EAWR_ROOT}/include")
target_link_libraries(eawr_godot PRIVATE godot-cpp eawr_godot_core)
get_target_property(GODOTCPP_SUFFIX godot-cpp GODOTCPP_SUFFIX)
set(PROJECT_BIN "${CMAKE_CURRENT_LIST_DIR}/project/bin")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/project/common")
configure_file("${EAWR_ROOT}/prototypes/common/scene.json"
    "${CMAKE_CURRENT_LIST_DIR}/project/common/scene.json" @ONLY NEWLINE_STYLE LF)
configure_file("${EAWR_ROOT}/tests/replay/fixtures/original-v1.eawr-replay"
    "${CMAKE_CURRENT_LIST_DIR}/project/common/original-v1.eawr-replay" COPYONLY)
set_target_properties(eawr_godot PROPERTIES
    CXX_VISIBILITY_PRESET "${GODOTCPP_SYMBOL_VISIBILITY}"
    VISIBILITY_INLINES_HIDDEN ON
    PREFIX "lib"
    OUTPUT_NAME "eawr_godot${GODOTCPP_SUFFIX}"
    LIBRARY_OUTPUT_DIRECTORY "$<1:${PROJECT_BIN}>"
    RUNTIME_OUTPUT_DIRECTORY "$<1:${PROJECT_BIN}>"
    PDB_OUTPUT_DIRECTORY "$<1:${PROJECT_BIN}>"
)

include(CTest)
if(BUILD_TESTING)
    add_executable(eawr_godot_shader_contract_test
        "${EAWR_ROOT}/tests/presentation/godot/shader_contract_test.cpp")
    target_include_directories(eawr_godot_shader_contract_test PRIVATE src)
    add_test(NAME eawr_godot_shader_contract COMMAND eawr_godot_shader_contract_test)
endif()
