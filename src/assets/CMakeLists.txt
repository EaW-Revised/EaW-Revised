add_library(
    eawr_assets STATIC
    animation.cpp
    map.cpp
    map_decode.cpp
    map_references.cpp
    mega_texture.cpp
    model.cpp
    texture.cpp
)
add_library(eawr::assets ALIAS eawr_assets)

target_include_directories(
    eawr_assets
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(eawr_assets PUBLIC eawr::core eawr::vfs eawr::data)
target_compile_features(eawr_assets PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_assets)
