# The effect name -> family table is generated from the committed P1-02
# descriptor bundle inventory at configure time, so the ledger's family column
# can never drift from plan/inventories/shader-corpus.json.
set(EAWR_SKY_SCAN_CORPUS "${PROJECT_SOURCE_DIR}/plan/inventories/shader-corpus.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${EAWR_SKY_SCAN_CORPUS}")
file(READ "${EAWR_SKY_SCAN_CORPUS}" eawr_sky_scan_corpus_json)
string(JSON eawr_sky_scan_bundle GET "${eawr_sky_scan_corpus_json}" bundle_id)
string(JSON eawr_sky_scan_effect_count LENGTH "${eawr_sky_scan_corpus_json}" effects)
set(eawr_sky_scan_rows "")
math(EXPR eawr_sky_scan_last "${eawr_sky_scan_effect_count} - 1")
foreach(eawr_sky_scan_index RANGE ${eawr_sky_scan_last})
    string(JSON eawr_sky_scan_name GET "${eawr_sky_scan_corpus_json}" effects ${eawr_sky_scan_index} name)
    string(JSON eawr_sky_scan_family GET "${eawr_sky_scan_corpus_json}" effects ${eawr_sky_scan_index} family)
    if(NOT eawr_sky_scan_name MATCHES "^[A-Za-z0-9_]+$" OR NOT eawr_sky_scan_family MATCHES "^[A-Z0-9_]+$")
        message(FATAL_ERROR "shader-corpus.json effect ${eawr_sky_scan_index} has an unexpected name or family")
    endif()
    string(APPEND eawr_sky_scan_rows "    DescriptorEffect{\"${eawr_sky_scan_name}\", \"${eawr_sky_scan_family}\"},\n")
endforeach()
set(eawr_sky_scan_header "${CMAKE_CURRENT_BINARY_DIR}/generated/sky_scan_descriptors.hpp")
file(CONFIGURE OUTPUT "${eawr_sky_scan_header}" CONTENT
"// Generated from plan/inventories/shader-corpus.json. Do not edit.
#pragma once
#include <array>
#include <string_view>
namespace sky_scan_descriptors {
struct DescriptorEffect final { std::string_view name; std::string_view family; };
inline constexpr std::string_view bundle_id = \"${eawr_sky_scan_bundle}\";
inline constexpr std::array<DescriptorEffect, ${eawr_sky_scan_effect_count}> effects{
${eawr_sky_scan_rows}};
} // namespace sky_scan_descriptors
" @ONLY)

add_executable(sky_scan main.cpp sky_scan.cpp sky_scan_report.cpp)
target_include_directories(sky_scan PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(sky_scan PRIVATE eawr::assets eawr::presentation eawr::vfs)
target_compile_features(sky_scan PRIVATE cxx_std_20)
eawr_target_project_warnings(sky_scan)
