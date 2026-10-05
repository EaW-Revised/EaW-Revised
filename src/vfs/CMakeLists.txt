include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")

add_subdirectory(
    "${PROJECT_SOURCE_DIR}/third_party/pugixml"
    "${PROJECT_BINARY_DIR}/third_party/pugixml"
    EXCLUDE_FROM_ALL
)

add_library(eawr_vfs STATIC ${EAWR_VFS_SOURCES})
add_library(eawr::vfs ALIAS eawr_vfs)

target_include_directories(
    eawr_vfs
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(eawr_vfs PUBLIC eawr::core PRIVATE pugixml::pugixml)
target_compile_features(eawr_vfs PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_vfs)
