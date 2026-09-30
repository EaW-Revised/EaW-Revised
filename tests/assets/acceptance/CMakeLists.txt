cmake_minimum_required(VERSION 3.24)
project(p0_06_acceptance LANGUAGES CXX)

if(NOT EAWR_ROOT)
    message(FATAL_ERROR "EAWR_ROOT must point at the repository root")
endif()
if(NOT EAWR_BUILD)
    message(FATAL_ERROR "EAWR_BUILD must point at the existing MSVC build tree")
endif()

add_executable(p0_06_asset_acceptance main.cpp)
target_compile_features(p0_06_asset_acceptance PRIVATE cxx_std_20)
target_include_directories(p0_06_asset_acceptance PRIVATE "${EAWR_ROOT}/include")
target_link_libraries(p0_06_asset_acceptance PRIVATE
    "${EAWR_BUILD}/src/assets/Release/eawr_assets.lib"
    "${EAWR_BUILD}/src/vfs/Release/eawr_vfs.lib"
    "${EAWR_BUILD}/src/core/Release/eawr_core.lib"
    "${EAWR_BUILD}/third_party/pugixml/Release/pugixml.lib"
)
