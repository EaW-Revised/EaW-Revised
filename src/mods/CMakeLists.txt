# The mod core (docs/design/mod-manager.md): records, discovery and fingerprints. Engine-free: no Godot, no
# simulation, no network; discovery reads listings and metadata. Only the profile API writes, under user data.
include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")

add_library(eawr_mods STATIC ${EAWR_MODS_SOURCES})
add_library(eawr::mods ALIAS eawr_mods)

target_include_directories(
    eawr_mods
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(eawr_mods PUBLIC eawr::core PRIVATE eawr::vfs)
if(WIN32)
    target_link_libraries(eawr_mods PRIVATE advapi32)
endif()
target_compile_features(eawr_mods PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_mods)
