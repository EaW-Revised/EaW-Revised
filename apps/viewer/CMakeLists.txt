cmake_minimum_required(VERSION 3.25)

project(EawrViewer VERSION 0.1.0 LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/EawrSymbols.cmake")
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
# The core and viewer exchange standard exceptions during startup and worker
# failure reporting. MSVC's disabled-exception STL uses a different exception
# base and layout; exporting it from the binding corrupts those boundaries.
if(MSVC)
    set(GODOTCPP_DISABLE_EXCEPTIONS OFF CACHE BOOL "Match the exception-enabled viewer core" FORCE)
    # Accepted prebuilt bindings may use the incompatible disabled-exception
    # STL. Rebuild the pinned sources rather than link an unverified ABI.
    if(EAWR_GODOT_CPP_LIBRARY)
        message(STATUS "MSVC viewer rebuilds pinned godot-cpp sources for exception ABI consistency")
        set(EAWR_GODOT_CPP_LIBRARY "")
    endif()
endif()
# Keep dependency headers outside the viewer's strict warning policy.
set(GODOTCPP_SYSTEM_HEADERS ON CACHE BOOL "Expose godot-cpp headers as SYSTEM" FORCE)
if(NOT WIN32)
    # A source-built binding links libstdc++ dynamically, like the accepted prebuilt
    # binding. With godot-cpp's static default a Clang 18 build crashed (SIGSEGV)
    # in libstdc++ stream code under the official Godot 4.7.2 Linux binary.
    set(GODOTCPP_USE_STATIC_CPP OFF CACHE BOOL "Link libstdc++ dynamically" FORCE)
endif()
# The owner's reserve switch: no local configure or build (of any target, godot-cpp too) while it is on.
include("${EAWR_ROOT}/cmake/EawrReserveGuard.cmake")
# An MSVC that drops the guard in front of a shift crashes the first Lua state (legacy #3930).
include("${EAWR_ROOT}/cmake/EawrMsvcGuard.cmake")
# Land skirmish (#3776 L3): the skirmish setup and LAN lobby list land maps and start a land battle. OFF in
# release builds, so the shipped setup screen stays byte-identical; ON only in the dev-land preset or with -D.
option(EAWR_ENABLE_LAND_SKIRMISH "Offer land maps in the skirmish setup and LAN lobby" OFF)
if(EAWR_ENABLE_LAND_SKIRMISH)
    add_compile_definitions(EAWR_LAND_SKIRMISH=1)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# All objects in the viewer link use the accepted binding's static CRT. Set the
# cache before source-built godot-cpp creates targets or sets its own default;
# relay TLS targets inherit this and GNS forwards it to its external builds.
if(MSVC)
    # The standalone viewer also compiles the core's large translation units.
    add_compile_options(/bigobj)
    # Dependencies such as Mbed TLS have an older policy baseline: keep runtime
    # selection enabled when their cmake_minimum_required resets policies.
    set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        CACHE STRING "Viewer MSVC runtime (matches the accepted godot-cpp binding)" FORCE)
endif()

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
            WINDOWS_ENABLED TYPED_METHOD_BIND NOMINMAX GDEXTENSION THREADS_ENABLED)
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
    if(MSVC)
        get_target_property(_eawr_godot_runtime godot-cpp MSVC_RUNTIME_LIBRARY)
        if(NOT _eawr_godot_runtime STREQUAL CMAKE_MSVC_RUNTIME_LIBRARY)
            message(FATAL_ERROR "godot-cpp and the viewer must use the same MSVC runtime")
        endif()
    endif()
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
include("${EAWR_ROOT}/src/mods/sources.cmake")
include("${EAWR_ROOT}/src/core/sources.cmake")
include("${EAWR_ROOT}/src/sim/sources.cmake")
include("${EAWR_ROOT}/src/sim/galactic/sources.cmake")
include("${EAWR_ROOT}/src/vfs/sources.cmake")
include("${EAWR_ROOT}/src/data/sources.cmake")
include("${EAWR_ROOT}/src/data/galactic/sources.cmake")
include("${EAWR_ROOT}/src/assets/sources.cmake")
include("${EAWR_ROOT}/src/data/ui/sources.cmake")
include("${EAWR_ROOT}/src/presentation/sources.cmake")
include("${EAWR_ROOT}/src/presentation/ui/sources.cmake")
include("${EAWR_ROOT}/src/scene/sources.cmake")
include("${EAWR_ROOT}/src/units/sources.cmake")
include("${EAWR_ROOT}/src/battle/sources.cmake")
include("${EAWR_ROOT}/src/skirmish/sources.cmake")
include("${EAWR_ROOT}/src/platform/sources.cmake")
include("${EAWR_ROOT}/src/platform/net/sources.cmake")
include("${EAWR_ROOT}/src/platform/galactic/sources.cmake")
include("${EAWR_ROOT}/src/sim/galactic/sources.cmake")
include("${EAWR_ROOT}/src/campaign/sources.cmake")
include("${EAWR_ROOT}/src/checkpoint/sources.cmake")
include("${EAWR_ROOT}/src/net/sources.cmake")
include("${EAWR_ROOT}/cmake/EawrGns.cmake")
include("${EAWR_ROOT}/src/script/sources.cmake")
include("${EAWR_ROOT}/apps/viewer/src/galactic/sources.cmake")
include("${EAWR_ROOT}/apps/viewer/src/sources.cmake")
include("${EAWR_ROOT}/src/presentation/godot/sources.cmake")
include("${EAWR_ROOT}/src/presentation/godot/ui/sources.cmake")

add_library(eawr_viewer_script STATIC
    ${EAWR_SCRIPT_NUMERIC_SOURCES}
    ${EAWR_SFLUA_HOOKS_SOURCES}
    ${EAWR_SFLUA_SOURCES}
    ${EAWR_SCRIPT_AUTHORITATIVE_SOURCES}
    ${EAWR_SCRIPT_GALACTIC_SOURCES}
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
# An MSVC that drops the guard before a shift (legacy #3930, #1287) compiles the table sizing at /Od.
eawr_msvc_guard_apply(eawr_viewer_script "${EAWR_ROOT}/src/script/sflua/upstream/ltable.cpp")

add_library(eawr_viewer_core STATIC
    ${EAWR_MODS_SOURCES}
    ${EAWR_CORE_SOURCES}
    ${EAWR_SIM_SOURCES}
    ${EAWR_SIM_GALACTIC_SOURCES}
    ${EAWR_VFS_SOURCES}
    ${EAWR_DATA_SOURCES}
    ${EAWR_GALACTIC_DATA_SOURCES}
    ${EAWR_ASSETS_SOURCES}
    ${EAWR_UI_DATA_SOURCES}
    ${EAWR_PRESENTATION_SOURCES}
    ${EAWR_UI_MODEL_SOURCES}
    ${EAWR_SCENE_SOURCES}
    ${EAWR_UNITS_SOURCES}
    ${EAWR_BATTLE_SOURCES}
    ${EAWR_SKIRMISH_SOURCES}
    ${EAWR_PLATFORM_NET_SOURCES}
    "${EAWR_ROOT}/src/platform/net/galactic_lan_match.cpp"
    "${EAWR_ROOT}/src/net/galactic_adapter.cpp"
    ${EAWR_PLATFORM_SOURCES}
    ${EAWR_PLATFORM_GALACTIC_SOURCES}
    ${EAWR_SIM_GALACTIC_SOURCES}
    ${EAWR_CAMPAIGN_SOURCES}
    ${EAWR_CHECKPOINT_SOURCES}
    ${EAWR_NET_SOURCES}
    ${EAWR_PLATFORM_LIVE_AI_SOURCES}
    "${EAWR_ROOT}/third_party/pugixml/src/pugixml.cpp"
    "${EAWR_VIEWER_XML_SCHEMA_INC}"
)
target_include_directories(eawr_viewer_core PUBLIC
    "${EAWR_ROOT}/src/platform/net"
    "${EAWR_ROOT}/include"
    "${EAWR_ROOT}/third_party/pugixml/src"
    "${EAWR_ROOT}/third_party/entt/single_include"
)
# src/sim/fog.cpp includes the private sim headers beside it; src/data/ui/command_bar.cpp
# includes src/data/xml_internal.hpp.
target_include_directories(eawr_viewer_core PRIVATE "${CMAKE_BINARY_DIR}/generated" "${EAWR_ROOT}/src/sim" "${EAWR_ROOT}/src/data")
target_include_directories(eawr_viewer_core SYSTEM PRIVATE "${EAWR_ROOT}/third_party/stb")
# The script library and the core use each other (the AI host reads the simulation; the live
# session hosts the scripts); CMake repeats the two static libraries on the link line.
target_link_libraries(eawr_viewer_core PUBLIC eawr_viewer_script ${EAWR_NET_PLATFORM_LIBRARIES})
target_link_libraries(eawr_viewer_core PUBLIC eawr::gns_dependency)
if(WIN32)
    target_link_libraries(eawr_viewer_core PUBLIC advapi32)
endif()
target_link_libraries(eawr_viewer_core PUBLIC eawr::relay)
target_link_libraries(eawr_viewer_script PUBLIC eawr_viewer_core)

add_library(eawr_viewer SHARED
    ${EAWR_VIEWER_SOURCES}
    ${EAWR_GODOT_SOURCES}
    ${EAWR_GODOT_UI_SOURCES}
)
target_include_directories(eawr_viewer PRIVATE
    src
    "${EAWR_ROOT}/src/platform/net"
    "${EAWR_ROOT}/include"
    "${EAWR_ROOT}/src/presentation/godot"
)
target_link_libraries(eawr_viewer PRIVATE godot-cpp eawr_viewer_core Threads::Threads)
if(WIN32)
    target_link_libraries(eawr_viewer PRIVATE dbghelp)
endif()
# FES-06: only the viewer's Debug configuration exposes debug menu entries.
target_compile_definitions(eawr_viewer PRIVATE $<$<CONFIG:Debug>:EAWR_VIEWER_DEBUG_BUILD=1>)
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
eawr_target_symbols(eawr_viewer)

if(MSVC)
    target_compile_options(eawr_viewer_core PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(eawr_viewer PRIVATE /W4 /WX /permissive- /EHsc)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        # godot-cpp __forceinline templates may not be inlined by MSVC.
        target_compile_options(eawr_viewer PRIVATE /wd4714)
    endif()
else()
    target_compile_options(eawr_viewer_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(eawr_viewer PRIVATE -Wall -Wextra -Wpedantic)
endif()
