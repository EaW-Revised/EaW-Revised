find_package(Threads REQUIRED)

# UI model (docs/ui/ui-layer.md section 3.1): layout, scaling, font sizes,
# font provisioning, the HUD view-model, the UI kit's theme model, and the input
# routing policy and command sink (UI-07).
add_library(eawr_ui_model STATIC layout.cpp fonts.cpp hud.cpp hud_shell.cpp shell_alpha.cpp theme.cpp
    ability_buttons.cpp battle_messages.cpp command_sink.cpp input_routing.cpp minimap.cpp perf_stats.cpp selection.cpp time_controls.cpp
    unit_cards.cpp world_ui.cpp)
add_library(eawr::ui_model ALIAS eawr_ui_model)
target_include_directories(eawr_ui_model PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>")
target_link_libraries(eawr_ui_model PUBLIC eawr::core eawr::ui_data eawr::presentation PRIVATE Threads::Threads)
target_compile_features(eawr_ui_model PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_ui_model)
