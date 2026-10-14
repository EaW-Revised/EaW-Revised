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

# The skirmish battle loading screen's model: the content built from the data, the weather and preview
# names, the history wrapping and the honest load progress (docs/behaviour/loading-screen.md).
add_executable(ui_loading_screen_tests loading_screen_tests.cpp)
target_link_libraries(ui_loading_screen_tests PRIVATE eawr::ui_model)
target_compile_features(ui_loading_screen_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(ui_loading_screen_tests)
add_test(NAME ui_loading_screen_contracts COMMAND ui_loading_screen_tests)

# The front end's navigation and enable-state model: the screen stack, Back and Escape, the
# gadget rules of the three menu dialogs and the fallback for a mod's unknown buttons.
add_executable(ui_front_end_tests front_end_tests.cpp)
target_link_libraries(ui_front_end_tests PRIVATE eawr::ui_model)
target_compile_features(ui_front_end_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(ui_front_end_tests)
add_test(NAME ui_front_end_contracts COMMAND ui_front_end_tests)

# The galactic custom-campaign staging model (MNU-34) and the Continue rule: the listing, the faction
# drop-down, Start's request and the options and difficulty gates.
add_executable(ui_campaign_setup_tests campaign_setup_tests.cpp)
target_link_libraries(ui_campaign_setup_tests PRIVATE eawr::ui_model)
target_compile_features(ui_campaign_setup_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(ui_campaign_setup_tests)
add_test(NAME ui_campaign_setup_contracts COMMAND ui_campaign_setup_tests)

# Message and confirm boxes (MNU-42): the dialog variant from the button count and kind, the button slots and
# the results of a press or Esc.
add_executable(ui_message_box_tests message_box_tests.cpp)
target_link_libraries(ui_message_box_tests PRIVATE eawr::ui_model)
target_compile_features(ui_message_box_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(ui_message_box_tests)
add_test(NAME ui_message_box_contracts COMMAND ui_message_box_tests)

# The LAN screens' model (MNU-26 to MNU-29): the game list's filter and sort, the join checks, the chat limits,
# the staging rows read from the dialog and the start and accept checks.
add_executable(ui_lan_lobby_tests lan_lobby_tests.cpp)
target_link_libraries(ui_lan_lobby_tests PRIVATE eawr::ui_model)
target_compile_features(ui_lan_lobby_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(ui_lan_lobby_tests)
add_test(NAME ui_lan_lobby_contracts COMMAND ui_lan_lobby_tests)

# The Load Game screen's model (MNU-39, MNU-41): replay header summaries, list order, the Load and Delete
# rules and a delete scoped to the replay store.
add_executable(ui_load_game_tests load_game_tests.cpp)
target_link_libraries(ui_load_game_tests PRIVATE eawr::ui_model)
target_compile_features(ui_load_game_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(ui_load_game_tests)
add_test(NAME ui_load_game_contracts COMMAND ui_load_game_tests)

# The Load Game screen's saved-game list (MNU-39, WGS-155/156/161): the rows the viewer's save-directory
# adapter builds from the checkpoint saves, their retail order and columns, and the store's list and delete.
add_executable(ui_saved_games_tests saved_games_tests.cpp ../../apps/viewer/src/saved_games.cpp)
target_link_libraries(ui_saved_games_tests PRIVATE eawr::ui_model eawr::checkpoint eawr::platform)
target_compile_features(ui_saved_games_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(ui_saved_games_tests)
add_test(NAME ui_saved_games_contracts COMMAND ui_saved_games_tests)

# The credits roll's model (MNU-07): the Credits_* constants, the text lines, the scroll and the end.
add_executable(ui_credits_tests credits_tests.cpp)
target_link_libraries(ui_credits_tests PRIVATE eawr::ui_model)
target_compile_features(ui_credits_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(ui_credits_tests)
add_test(NAME ui_credits_contracts COMMAND ui_credits_tests)

# The skirmish staging screen's text and start blockers: captions are text-database keys, the screen's source
# holds no caption literal, the land game types stay listed and disabled (docs/ui/menu-screens.md MNU-21, MNU-22).
add_executable(ui_skirmish_setup_tests skirmish_setup_tests.cpp)
target_link_libraries(ui_skirmish_setup_tests PRIVATE eawr::ui_model)
target_compile_features(ui_skirmish_setup_tests PRIVATE cxx_std_20)
target_compile_definitions(ui_skirmish_setup_tests PRIVATE
    EAWR_VIEWER_SOURCE_DIR="${PROJECT_SOURCE_DIR}/apps/viewer/src")
eawr_target_project_warnings(ui_skirmish_setup_tests)
add_test(NAME ui_skirmish_setup_contracts COMMAND ui_skirmish_setup_tests)

# Tier labels (cmake/EawrTestTiers.cmake).
add_executable(ui_cursor_tests cursor_tests.cpp)
target_link_libraries(ui_cursor_tests PRIVATE eawr::ui_model)
target_compile_features(ui_cursor_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(ui_cursor_tests)
add_test(NAME ui_cursor_contracts COMMAND ui_cursor_tests)

# #2927: the player settings record (defaults, text round trip, damaged files, clamping, option table).
add_executable(ui_settings_tests settings_tests.cpp)
target_link_libraries(ui_settings_tests PRIVATE eawr::ui_model)
target_compile_features(ui_settings_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(ui_settings_tests)
add_test(NAME ui_settings_contracts COMMAND ui_settings_tests)

# #2935: the player profile record (defaults, round trip, damaged files, non-ASCII names and paths, write-back).
add_executable(ui_player_profile_tests player_profile_tests.cpp)
target_link_libraries(ui_player_profile_tests PRIVATE eawr::ui_model)
target_compile_features(ui_player_profile_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(ui_player_profile_tests)
add_test(NAME ui_player_profile_contracts COMMAND ui_player_profile_tests)

eawr_label_tests()
