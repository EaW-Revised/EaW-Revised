add_library(eawr_scene STATIC idle_tags.cpp scene.cpp scene_assets.cpp scene_build.cpp scene_evidence.cpp space_population.cpp)
add_library(eawr::scene ALIAS eawr_scene)

target_include_directories(
    eawr_scene
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(eawr_scene PUBLIC eawr::assets eawr::data eawr::sim eawr::vfs eawr::core)
target_compile_features(eawr_scene PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_scene)
