include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")

add_library(eawr_assets STATIC ${EAWR_ASSETS_SOURCES})
add_library(eawr::assets ALIAS eawr_assets)

target_include_directories(
    eawr_assets
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(eawr_assets PUBLIC eawr::core eawr::vfs eawr::data)
target_compile_features(eawr_assets PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_assets)
