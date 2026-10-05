add_executable(ui_data_tests main.cpp text_database_tests.cpp dialog_catalog_tests.cpp
    dialog_catalog_fixture.cpp
    dialog_script_tests.cpp
    dialog_skin_tests.cpp
    dialog_corpus_tests.cpp command_bar_tests.cpp shell_anchor_tests.cpp movie_tests.cpp)
target_link_libraries(ui_data_tests PRIVATE eawr::ui_data)
target_compile_features(ui_data_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(ui_data_tests)
add_test(NAME ui_data_contracts COMMAND ui_data_tests)

add_executable(ui_model_tests model_main.cpp layout_tests.cpp font_tests.cpp hud_tests.cpp hud_shell_tests.cpp theme_tests.cpp)
target_link_libraries(ui_model_tests PRIVATE eawr::ui_model)
target_compile_features(ui_model_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(ui_model_tests)
add_test(NAME ui_model_contracts COMMAND ui_model_tests)

# UI-07 (#304): the command sink (UI-C1, UI-C2) against a tactical session at 1/2/4
# workers, and the input routing policy (UI-I1, UI-I3).
find_package(Threads REQUIRED)
add_executable(ui_input_command_tests input_command_tests.cpp)
target_link_libraries(ui_input_command_tests PRIVATE eawr::ui_model eawr::sim eawr::platform Threads::Threads)
target_compile_features(ui_input_command_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(ui_input_command_tests)
add_test(NAME ui_input_command_contracts COMMAND ui_input_command_tests)

# UI-05 (#191): the font extraction tool on synthetic fonts; the real-executable
# checks run when EAWR_EAW_GAME_ROOT is set.
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME ui_font_extraction
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_extract_eaw_fonts.py"
)

# #237: the user-side HUD movie conversion with stand-in tools; it packs a
# generated clip for real when an FFmpeg with libtheora is on PATH.
add_test(
    NAME ui_movie_conversion
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_convert_hud_movie.py"
)

# #82 P2-19: battle picking, click/box/type/group selection, the tactical overview, and selection
# never reaching the replay.
add_executable(ui_battle_input_tests battle_input_tests.cpp)
target_link_libraries(ui_battle_input_tests PRIVATE eawr::ui_model eawr::sim)
target_compile_features(ui_battle_input_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(ui_battle_input_tests)
add_test(NAME ui_battle_input_contracts COMMAND ui_battle_input_tests)

# #425 P2-20b: the command bar's unit cards (grouping, stacking, bars, borders, card clicks).
add_executable(ui_unit_cards_tests unit_cards_tests.cpp)
target_link_libraries(ui_unit_cards_tests PRIVATE eawr::ui_model eawr::sim)
target_compile_features(ui_unit_cards_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(ui_unit_cards_tests)
add_test(NAME ui_unit_cards_contracts COMMAND ui_unit_cards_tests)
# #530: the command bar's station production (build buttons, build queues, reinforcement pool).
add_executable(ui_production_tests production_tests.cpp)
target_link_libraries(ui_production_tests PRIVATE eawr::ui_model eawr::sim)
target_compile_features(ui_production_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(ui_production_tests)
add_test(NAME ui_production_contracts COMMAND ui_production_tests)
add_executable(ui_world_ui_tests world_ui_tests.cpp)
target_link_libraries(ui_world_ui_tests PRIVATE eawr::ui_model)
target_compile_features(ui_world_ui_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(ui_world_ui_tests)
add_test(NAME ui_world_ui_contracts COMMAND ui_world_ui_tests)

# #459: the tactical time panel's model (pause, fast forward, speed steps, the time track).
add_executable(ui_time_controls_tests time_controls_tests.cpp)
target_link_libraries(ui_time_controls_tests PRIVATE eawr::ui_model)
target_compile_features(ui_time_controls_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(ui_time_controls_tests)
add_test(NAME ui_time_controls_contracts COMMAND ui_time_controls_tests)

# #848: the battle UI in the tactical overview levels (what hides, the level-change fade).
add_executable(ui_overview_ui_tests overview_ui_tests.cpp)
target_link_libraries(ui_overview_ui_tests PRIVATE eawr::ui_model)
target_compile_features(ui_overview_ui_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(ui_overview_ui_tests)
add_test(NAME ui_overview_ui_contracts COMMAND ui_overview_ui_tests)

# #558: the performance overlay's model (frame times, tick costs, the rolling window, graph columns).
add_executable(ui_perf_stats_tests perf_stats_tests.cpp)
target_link_libraries(ui_perf_stats_tests PRIVATE eawr::ui_model)
target_compile_features(ui_perf_stats_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(ui_perf_stats_tests)
add_test(NAME ui_perf_stats_contracts COMMAND ui_perf_stats_tests)

# #455: the tactical minimap's model (settings, mapping, blips, the camera guide, the fog layer).
add_executable(ui_minimap_tests minimap_tests.cpp)
target_link_libraries(ui_minimap_tests PRIVATE eawr::ui_model eawr::sim)
target_compile_features(ui_minimap_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(ui_minimap_tests)
add_test(NAME ui_minimap_contracts COMMAND ui_minimap_tests)

# #454: the command bar's ability buttons (buttons, card marks, clicks, hotkeys).
add_executable(ui_ability_buttons_tests ability_buttons_tests.cpp)
target_link_libraries(ui_ability_buttons_tests PRIVATE eawr::ui_model eawr::sim)
target_compile_features(ui_ability_buttons_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(ui_ability_buttons_tests)
add_test(NAME ui_ability_buttons_contracts COMMAND ui_ability_buttons_tests)

# Tier labels (cmake/EawrTestTiers.cmake).
add_executable(ui_cursor_tests cursor_tests.cpp)
target_link_libraries(ui_cursor_tests PRIVATE eawr::ui_model)
target_compile_features(ui_cursor_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(ui_cursor_tests)
add_test(NAME ui_cursor_contracts COMMAND ui_cursor_tests)

eawr_label_tests()
