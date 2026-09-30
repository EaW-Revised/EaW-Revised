cmake_minimum_required(VERSION 3.25)
project(p007_acceptance LANGUAGES CXX)

set(EAWR_SOURCE_ROOT "" CACHE PATH "Repository source root")
set(EAWR_BUILD_ROOT "" CACHE PATH "Existing eawr build root")
set(EAWR_CONFIG "Release" CACHE STRING "Existing multi-config build configuration")
if(NOT EAWR_SOURCE_ROOT OR NOT EAWR_BUILD_ROOT)
    message(FATAL_ERROR "EAWR_SOURCE_ROOT and EAWR_BUILD_ROOT are required")
endif()

add_executable(p007_acceptance acceptance_harness.cpp)
target_compile_features(p007_acceptance PRIVATE cxx_std_20)
target_include_directories(p007_acceptance PRIVATE "${EAWR_SOURCE_ROOT}/include")

add_executable(p007_instruction_width instruction_width_probe.cpp)
target_include_directories(p007_instruction_width PRIVATE
    "${EAWR_SOURCE_ROOT}/third_party/lua/include"
    "${EAWR_SOURCE_ROOT}/third_party/lua/src"
)

if(WIN32)
    set(_script "${EAWR_BUILD_ROOT}/src/script/${EAWR_CONFIG}/eawr_script.lib")
    set(_vfs "${EAWR_BUILD_ROOT}/src/vfs/${EAWR_CONFIG}/eawr_vfs.lib")
    set(_core "${EAWR_BUILD_ROOT}/src/core/${EAWR_CONFIG}/eawr_core.lib")
    set(_lua "${EAWR_BUILD_ROOT}/third_party/lua/${EAWR_CONFIG}/eawr_lua_502.lib")
    set(_pugixml "${EAWR_BUILD_ROOT}/third_party/pugixml/${EAWR_CONFIG}/pugixml.lib")
else()
    set(_script "${EAWR_BUILD_ROOT}/src/script/libeawr_script.a")
    set(_vfs "${EAWR_BUILD_ROOT}/src/vfs/libeawr_vfs.a")
    set(_core "${EAWR_BUILD_ROOT}/src/core/libeawr_core.a")
    set(_lua "${EAWR_BUILD_ROOT}/third_party/lua/libeawr_lua_502.a")
    set(_pugixml "${EAWR_BUILD_ROOT}/third_party/pugixml/libpugixml.a")
endif()
foreach(_required IN ITEMS _script _vfs _core _lua _pugixml)
    if(NOT EXISTS "${${_required}}")
        message(FATAL_ERROR "Missing imported library ${${_required}}")
    endif()
endforeach()

target_link_libraries(p007_acceptance PRIVATE
    "${_script}"
    "${_vfs}"
    "${_core}"
    "${_lua}"
    "${_pugixml}"
)
if(WIN32)
    target_link_libraries(p007_acceptance PRIVATE shlwapi)
endif()
