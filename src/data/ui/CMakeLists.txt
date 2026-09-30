# Engine-free UI data loaders (docs/ui/ui-layer.md, section 3.1): text
# database, dialog catalogue, command bar, shell anchors and sfnt faces.
add_library(eawr_ui_data STATIC text_database.cpp dialog_catalog.cpp dialog_script.cpp command_bar.cpp shell_anchors.cpp
    sfnt.cpp movie.cpp)
add_library(eawr::ui_data ALIAS eawr_ui_data)

target_include_directories(
    eawr_ui_data
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    # The command bar reuses the hardened XML parse of eawr_data.
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/data"
)
target_link_libraries(eawr_ui_data PUBLIC eawr::core eawr::vfs eawr::assets eawr::data PRIVATE pugixml::pugixml)
target_compile_features(eawr_ui_data PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_ui_data)
