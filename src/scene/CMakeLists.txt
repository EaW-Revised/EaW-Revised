include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")

add_library(eawr_scene STATIC ${EAWR_SCENE_SOURCES})
add_library(eawr::scene ALIAS eawr_scene)

target_include_directories(
    eawr_scene
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(eawr_scene PUBLIC eawr::assets eawr::data eawr::sim eawr::vfs eawr::core)
target_compile_features(eawr_scene PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_scene)
