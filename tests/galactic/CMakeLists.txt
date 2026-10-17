# M3 galactic tests (docs/galactic-architecture.md, testing).
add_executable(galactic_ability_host_tests ability_host_tests.cpp)
target_link_libraries(galactic_ability_host_tests PRIVATE eawr::campaign eawr::checkpoint eawr::platform)
target_compile_features(galactic_ability_host_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ability_host_tests)
add_test(NAME galactic_ability_host_contracts COMMAND galactic_ability_host_tests)
add_executable(galactic_ability_theft_tests ability_theft_tests.cpp)
target_link_libraries(galactic_ability_theft_tests PRIVATE eawr::campaign eawr::checkpoint eawr::platform)
target_compile_features(galactic_ability_theft_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ability_theft_tests)
add_test(NAME galactic_ability_theft_contracts COMMAND galactic_ability_theft_tests)
add_executable(galactic_ability_corruption_tests ability_corruption_tests.cpp)
target_link_libraries(galactic_ability_corruption_tests PRIVATE eawr::campaign eawr::checkpoint eawr::platform)
target_compile_features(galactic_ability_corruption_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ability_corruption_tests)
add_test(NAME galactic_ability_corruption_contracts COMMAND galactic_ability_corruption_tests)
add_executable(galactic_ability_ruins_tests ability_ruins_tests.cpp)
target_link_libraries(galactic_ability_ruins_tests PRIVATE eawr::campaign eawr::checkpoint eawr::platform)
target_compile_features(galactic_ability_ruins_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ability_ruins_tests)
add_test(NAME galactic_ability_ruins_contracts COMMAND galactic_ability_ruins_tests)
add_executable(galactic_ability_hack_tests ability_hack_tests.cpp)
target_link_libraries(galactic_ability_hack_tests PRIVATE eawr::campaign eawr::checkpoint eawr::platform)
target_compile_features(galactic_ability_hack_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ability_hack_tests)
add_test(NAME galactic_ability_hack_contracts COMMAND galactic_ability_hack_tests)
add_executable(galactic_ability_destruction_tests ability_destruction_tests.cpp)
target_link_libraries(galactic_ability_destruction_tests PRIVATE eawr::campaign eawr::checkpoint eawr::platform)
target_compile_features(galactic_ability_destruction_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ability_destruction_tests)
add_test(NAME galactic_ability_destruction_contracts COMMAND galactic_ability_destruction_tests)
add_executable(galactic_corruption_model_tests corruption_model_tests.cpp)
target_link_libraries(galactic_corruption_model_tests PRIVATE eawr::sim_galactic)
target_compile_features(galactic_corruption_model_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_corruption_model_tests)
add_test(NAME galactic_corruption_model_contracts COMMAND galactic_corruption_model_tests)
add_executable(galactic_s2_loop_tests s2_loop_tests.cpp)
target_include_directories(galactic_s2_loop_tests PRIVATE "${PROJECT_SOURCE_DIR}/apps/viewer/src")
target_link_libraries(galactic_s2_loop_tests PRIVATE eawr::campaign eawr::platform eawr::presentation)
target_compile_features(galactic_s2_loop_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_s2_loop_tests)
add_test(NAME galactic_s2_loop COMMAND galactic_s2_loop_tests)
set_tests_properties(galactic_s2_loop PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME galactic_purchase_ground_roster COMMAND galactic_s2_loop_tests --purchase-only)
set_tests_properties(galactic_purchase_ground_roster PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_executable(galactic_live_battle_ai_tests live_battle_ai_tests.cpp)
target_include_directories(galactic_live_battle_ai_tests PRIVATE "${PROJECT_SOURCE_DIR}/apps/viewer/src")
target_link_libraries(galactic_live_battle_ai_tests PRIVATE eawr::campaign eawr::platform eawr::presentation)
target_compile_features(galactic_live_battle_ai_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_live_battle_ai_tests)
eawr_add_game_data_test(NAME galactic_live_battle_ai_native_off COMMAND galactic_live_battle_ai_tests)
set_tests_properties(galactic_live_battle_ai_native_off PROPERTIES LABELS game-data SKIP_REGULAR_EXPRESSION "SKIPPED:")
eawr_add_game_data_test(NAME galactic_live_battle_ai_native_on COMMAND galactic_live_battle_ai_tests --galactic-ai-on)
set_tests_properties(galactic_live_battle_ai_native_on PROPERTIES LABELS game-data SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_executable(galactic_live_campaign_setup_tests live_campaign_setup_tests.cpp)
target_link_libraries(galactic_live_campaign_setup_tests PRIVATE eawr::campaign eawr::checkpoint eawr::platform eawr::ui_model)
target_include_directories(galactic_live_campaign_setup_tests PRIVATE "${PROJECT_SOURCE_DIR}/apps/viewer/src")
target_compile_features(galactic_live_campaign_setup_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_live_campaign_setup_tests)
add_test(NAME galactic_live_campaign_setup_contracts COMMAND galactic_live_campaign_setup_tests)
eawr_add_game_data_test(NAME galactic_production_domain_stock COMMAND galactic_live_campaign_setup_tests --production-domains)
set_tests_properties(galactic_production_domain_stock PROPERTIES LABELS game-data SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME galactic_starting_tech_stock COMMAND galactic_live_campaign_setup_tests --starting-tech)
set_tests_properties(galactic_starting_tech_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME galactic_accepted_starting_tech_stock COMMAND galactic_live_campaign_setup_tests --accepted-starting-tech)
set_tests_properties(galactic_accepted_starting_tech_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
eawr_add_game_data_test(NAME galactic_origin_starbase_stock COMMAND galactic_live_campaign_setup_tests --origin-starbases)
set_tests_properties(galactic_origin_starbase_stock PROPERTIES LABELS game-data SKIP_REGULAR_EXPRESSION "SKIPPED:")
eawr_add_game_data_test(NAME galactic_unscouted_intel_stock COMMAND galactic_live_campaign_setup_tests --unscouted-intel)
set_tests_properties(galactic_unscouted_intel_stock PROPERTIES LABELS game-data SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_executable(galactic_income_tests income_tests.cpp)
target_link_libraries(galactic_income_tests PRIVATE eawr::campaign eawr::checkpoint eawr::platform)
target_compile_features(galactic_income_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_income_tests)
add_test(NAME galactic_income_contracts COMMAND galactic_income_tests)
add_executable(galactic_income_blocker_tests income_blocker_tests.cpp)
target_link_libraries(galactic_income_blocker_tests PRIVATE eawr::campaign eawr::checkpoint eawr::platform)
target_compile_features(galactic_income_blocker_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_income_blocker_tests)
add_test(NAME galactic_income_blocker_contracts COMMAND galactic_income_blocker_tests)
add_executable(galactic_map_input_tests
    map_input_tests.cpp
    "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/galaxy_view.cpp"
    "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/galaxy_snapshot_stub.cpp"
)
target_include_directories(galactic_map_input_tests PRIVATE "${PROJECT_SOURCE_DIR}/apps/viewer/src")
target_link_libraries(galactic_map_input_tests PRIVATE eawr::sim_galactic)
target_compile_features(galactic_map_input_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_map_input_tests)
add_test(NAME galactic_map_input_contracts COMMAND galactic_map_input_tests)
eawr_add_python_contract(python_presentation_renderer_galaxy_input_runtime tests.presentation.renderer.test_viewer_galaxy_input_runtime)
set_tests_properties(python_presentation_renderer_galaxy_input_runtime PROPERTIES LABELS gpu)
add_executable(galactic_audio_tests audio_tests.cpp)
target_link_libraries(galactic_audio_tests PRIVATE eawr::presentation)
target_compile_features(galactic_audio_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_audio_tests)
add_test(NAME galactic_audio_contracts COMMAND galactic_audio_tests)
add_executable(galactic_audio_feed_tests audio_feed_tests.cpp)
target_link_libraries(galactic_audio_feed_tests PRIVATE eawr::sim_galactic eawr::platform eawr::presentation)
target_compile_features(galactic_audio_feed_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_audio_feed_tests)
add_test(NAME galactic_audio_feed_contracts COMMAND galactic_audio_feed_tests)
eawr_add_python_contract(python_presentation_renderer_galactic_audio tests.presentation.renderer.test_galactic_audio)
set_tests_properties(python_presentation_renderer_galactic_audio PROPERTIES LABELS gpu)
add_executable(galactic_auto_resolve_outcome_tests auto_resolve_outcome_tests.cpp)
target_link_libraries(galactic_auto_resolve_outcome_tests PRIVATE eawr::campaign eawr::platform eawr::checkpoint)
target_compile_features(galactic_auto_resolve_outcome_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_auto_resolve_outcome_tests)
add_test(NAME galactic_auto_resolve_outcome COMMAND galactic_auto_resolve_outcome_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/outcome-corpus/space-loss.eawr-gcreplay")
add_executable(galactic_observer_tests observer_tests.cpp)
target_link_libraries(galactic_observer_tests PRIVATE eawr::campaign eawr::checkpoint eawr::platform)
target_compile_features(galactic_observer_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_observer_tests)
add_test(NAME galactic_observer COMMAND galactic_observer_tests)

add_executable(galactic_fleet_intelligence_tests fleet_intelligence_tests.cpp)
target_link_libraries(galactic_fleet_intelligence_tests PRIVATE eawr::sim_galactic eawr::platform)
target_compile_features(galactic_fleet_intelligence_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_fleet_intelligence_tests)
add_test(NAME galactic_fleet_intelligence COMMAND galactic_fleet_intelligence_tests)

add_executable(galactic_fleet_hud_fixture_tests fleet_hud_fixture_tests.cpp
    "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/galaxy_view.cpp"
    "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/galaxy_snapshot_stub.cpp")
target_link_libraries(galactic_fleet_hud_fixture_tests PRIVATE eawr::sim_galactic eawr::platform)
target_include_directories(galactic_fleet_hud_fixture_tests PRIVATE "${PROJECT_SOURCE_DIR}/apps/viewer/src")
target_compile_features(galactic_fleet_hud_fixture_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_fleet_hud_fixture_tests)
add_test(NAME galactic_fleet_hud_fixture COMMAND galactic_fleet_hud_fixture_tests)

add_executable(galactic_weapon_action_live_fixture_tests weapon_action_live_fixture_tests.cpp
    "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/galaxy_view.cpp"
    "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/galaxy_snapshot_stub.cpp")
target_link_libraries(galactic_weapon_action_live_fixture_tests PRIVATE eawr::campaign eawr::sim_galactic eawr::platform)
target_include_directories(galactic_weapon_action_live_fixture_tests PRIVATE "${PROJECT_SOURCE_DIR}/apps/viewer/src")
target_compile_features(galactic_weapon_action_live_fixture_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_weapon_action_live_fixture_tests)
add_test(NAME galactic_weapon_action_live_fixture COMMAND galactic_weapon_action_live_fixture_tests)

eawr_add_python_contract(python_presentation_renderer_galactic_fleet_runtime tests.presentation.renderer.test_viewer_galactic_fleet_runtime)
set_tests_properties(python_presentation_renderer_galactic_fleet_runtime PROPERTIES LABELS gpu)

eawr_add_python_contract(python_presentation_renderer_galactic_radar_runtime tests.presentation.renderer.test_viewer_galactic_radar_runtime)
set_tests_properties(python_presentation_renderer_galactic_radar_runtime PROPERTIES LABELS gpu)

eawr_add_python_contract(python_presentation_renderer_command_bar_offsets_runtime tests.presentation.renderer.test_viewer_command_bar_offsets_runtime)
set_tests_properties(python_presentation_renderer_command_bar_offsets_runtime PROPERTIES LABELS gpu)

add_executable(galactic_conflict_policy_tests conflict_policy_tests.cpp)
target_link_libraries(galactic_conflict_policy_tests PRIVATE eawr::campaign)
target_compile_features(galactic_conflict_policy_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_conflict_policy_tests)
add_test(NAME galactic_conflict_policy_contracts COMMAND galactic_conflict_policy_tests)

add_executable(galactic_auto_resolve_round_tests auto_resolve_round_tests.cpp)
target_link_libraries(galactic_auto_resolve_round_tests PRIVATE eawr::campaign eawr::platform eawr::script_foc)
target_compile_features(galactic_auto_resolve_round_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_auto_resolve_round_tests)
add_test(NAME galactic_auto_resolve_round COMMAND galactic_auto_resolve_round_tests)
add_test(NAME galactic_auto_resolve_round_stock COMMAND galactic_auto_resolve_round_tests --installed)
set_tests_properties(galactic_auto_resolve_round_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")

add_executable(galactic_auto_resolve_strength_tests auto_resolve_strength_tests.cpp)
target_link_libraries(galactic_auto_resolve_strength_tests PRIVATE eawr::campaign eawr::platform)
target_compile_features(galactic_auto_resolve_strength_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_auto_resolve_strength_tests)
add_test(NAME galactic_auto_resolve_strength COMMAND galactic_auto_resolve_strength_tests)
add_test(NAME galactic_auto_resolve_strength_stock COMMAND galactic_auto_resolve_strength_tests --installed)
set_tests_properties(galactic_auto_resolve_strength_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")

# FT-06: the two-peer lockstep loopback gate, over the committed tactical replay corpus first.
add_executable(gc_lockstep_loopback_tests lockstep_loopback_tests.cpp)
target_link_libraries(gc_lockstep_loopback_tests PRIVATE eawr::sim eawr::platform)
target_compile_features(gc_lockstep_loopback_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(gc_lockstep_loopback_tests)
add_test(
    NAME gc_lockstep_loopback
    COMMAND gc_lockstep_loopback_tests
        "${PROJECT_SOURCE_DIR}/tests/fidelity/fixtures"
        "${PROJECT_SOURCE_DIR}/tests/replay/fixtures"
        "${PROJECT_SOURCE_DIR}/tests/skirmish/fixtures"
)

# Engine-free view contracts over immutable live snapshots.
add_executable(galactic_viewer_view_tests
    galactic_viewer_view_tests.cpp
    "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/galaxy_view.cpp"
    "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/galaxy_snapshot_stub.cpp"
    "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/planet_hud.cpp"
)
target_include_directories(galactic_viewer_view_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/apps/viewer/src"
)
target_compile_features(galactic_viewer_view_tests PRIVATE cxx_std_20)
target_link_libraries(galactic_viewer_view_tests PRIVATE eawr::sim_galactic eawr::platform)
eawr_target_project_warnings(galactic_viewer_view_tests)
add_test(NAME galactic_viewer_view_contracts COMMAND galactic_viewer_view_tests)

add_executable(galactic_scene_tests
    galactic_scene_tests.cpp
    "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/scene_content.cpp"
)
target_include_directories(galactic_scene_tests PRIVATE "${PROJECT_SOURCE_DIR}/apps/viewer/src")
target_link_libraries(galactic_scene_tests PRIVATE eawr::presentation eawr::ui_data eawr::ui_model)
target_compile_features(galactic_scene_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_scene_tests)
add_test(NAME galactic_scene_contracts COMMAND galactic_scene_tests)
eawr_add_python_contract(python_presentation_renderer_galaxy_scene_runtime tests.presentation.renderer.test_viewer_galaxy_scene_runtime)
set_tests_properties(python_presentation_renderer_galaxy_scene_runtime PROPERTIES LABELS gpu)
eawr_add_python_contract(python_presentation_renderer_galaxy_start_runtime tests.presentation.renderer.test_viewer_galaxy_start_runtime)
set_tests_properties(python_presentation_renderer_galaxy_start_runtime PROPERTIES LABELS gpu)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME galactic_viewer_mode_contracts
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_galactic_viewer_mode.py"
)

find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

add_executable(galactic_session_tests session_tests.cpp)
target_link_libraries(galactic_session_tests PRIVATE eawr::sim_galactic eawr::platform)
target_compile_features(galactic_session_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_session_tests)
add_test(NAME galactic_session_contracts COMMAND galactic_session_tests)

add_executable(galactic_replay_tests replay_tests.cpp)
target_link_libraries(galactic_replay_tests PRIVATE eawr::sim_galactic eawr::platform)
target_compile_features(galactic_replay_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_replay_tests)
add_test(NAME galactic_replay_contracts COMMAND galactic_replay_tests)

add_executable(galactic_clock_tests clock_tests.cpp)
target_link_libraries(galactic_clock_tests PRIVATE eawr::sim_galactic eawr::platform)
target_compile_features(galactic_clock_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_clock_tests)
add_test(NAME galactic_clock_contracts COMMAND galactic_clock_tests)

add_executable(galactic_game_session_tests game_session_tests.cpp)
target_link_libraries(galactic_game_session_tests PRIVATE eawr::campaign eawr::platform)
target_compile_features(galactic_game_session_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_game_session_tests)
add_test(NAME galactic_game_session_contracts COMMAND galactic_game_session_tests)

add_executable(galactic_tactical_return_tests tactical_return_tests.cpp)
target_link_libraries(galactic_tactical_return_tests PRIVATE eawr::campaign eawr::platform)
target_compile_features(galactic_tactical_return_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_tactical_return_tests)
add_test(NAME galactic_tactical_return COMMAND galactic_tactical_return_tests)
eawr_add_game_data_test(NAME galactic_tactical_return_stock COMMAND galactic_tactical_return_tests --installed)
set_tests_properties(galactic_tactical_return_stock PROPERTIES LABELS game-data)

add_executable(galactic_live_game_session_tests live_game_session_tests.cpp)
target_link_libraries(galactic_live_game_session_tests PRIVATE eawr::campaign eawr::platform)
target_compile_features(galactic_live_game_session_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_live_game_session_tests)
add_test(NAME galactic_live_game_session_contracts COMMAND galactic_live_game_session_tests)

add_executable(galactic_campaign_score_tests campaign_score_tests.cpp)
target_include_directories(galactic_campaign_score_tests PRIVATE "${PROJECT_SOURCE_DIR}/apps/viewer/src")
target_link_libraries(galactic_campaign_score_tests PRIVATE eawr::campaign eawr::platform eawr::data eawr::vfs)
target_compile_features(galactic_campaign_score_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_campaign_score_tests)
add_test(NAME galactic_campaign_score_contracts COMMAND galactic_campaign_score_tests)

add_executable(galactic_campaign_scoring_tests campaign_scoring_tests.cpp)
target_link_libraries(galactic_campaign_scoring_tests PRIVATE eawr::campaign eawr::platform eawr::data eawr::vfs)
target_compile_features(galactic_campaign_scoring_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_campaign_scoring_tests)
add_test(NAME galactic_campaign_scoring_contracts COMMAND galactic_campaign_scoring_tests)
eawr_add_game_data_test(NAME galactic_campaign_scoring_stock COMMAND galactic_campaign_scoring_tests --installed)
set_tests_properties(galactic_campaign_scoring_stock PROPERTIES LABELS game-data)

add_executable(galactic_live_save_tests live_save_tests.cpp)
target_link_libraries(galactic_live_save_tests PRIVATE eawr::checkpoint eawr::platform)
target_compile_features(galactic_live_save_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_live_save_tests)
add_test(NAME galactic_live_save_roundtrip COMMAND galactic_live_save_tests "${CMAKE_CURRENT_BINARY_DIR}/live-save-contract")
eawr_add_python_contract(python_presentation_renderer_galactic_save_runtime tests.presentation.renderer.test_viewer_galactic_save_runtime)
set_tests_properties(python_presentation_renderer_galactic_save_runtime PROPERTIES LABELS gpu)

add_executable(galactic_scripted_session_tests scripted_session_tests.cpp)
target_link_libraries(galactic_scripted_session_tests PRIVATE eawr::script_galactic eawr::platform)
target_compile_features(galactic_scripted_session_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_scripted_session_tests)
add_test(NAME galactic_scripted_session_contracts COMMAND galactic_scripted_session_tests)
add_test(NAME galactic_lua_api_inventory_contracts
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_lua_api_inventory.py"
)

# sim_headless --galactic and --galactic-replay over the synthetic campaign fixtures.
add_executable(galactic_game_checkpoint_tests game_checkpoint_tests.cpp)
target_link_libraries(galactic_game_checkpoint_tests PRIVATE eawr::checkpoint eawr::platform)
target_compile_features(galactic_game_checkpoint_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_game_checkpoint_tests)
add_test(NAME galactic_timeline_contracts COMMAND galactic_game_checkpoint_tests --timeline)
add_test(NAME galactic_game_checkpoint_contracts COMMAND galactic_game_checkpoint_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/corpus")

add_test(
    NAME galactic_cli_contract
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/galactic_cli_check.py"
        --program "$<TARGET_FILE:sim_headless>"
        --fixtures "${CMAKE_CURRENT_LIST_DIR}/fixtures"
)

add_executable(galactic_shared_control_tests shared_control_tests.cpp)
target_link_libraries(galactic_shared_control_tests PRIVATE eawr::skirmish eawr::platform)
target_compile_features(galactic_shared_control_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_shared_control_tests)
add_test(NAME galactic_shared_control COMMAND galactic_shared_control_tests)
add_test(NAME galactic_shared_control_m2 COMMAND galactic_shared_control_tests --installed)
set_tests_properties(galactic_shared_control_m2 PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")

# Author synthetic fixtures with the same public GameSession API as the CLI runner.
add_executable(galactic_replay_corpus_generate replay_corpus_generate.cpp)
target_link_libraries(galactic_replay_corpus_generate PRIVATE eawr::campaign)
target_compile_features(galactic_replay_corpus_generate PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_replay_corpus_generate)

add_test(NAME galactic_replay_corpus
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/replay_corpus_check.py"
        --program "$<TARGET_FILE:sim_headless>"
        --corpus "${CMAKE_CURRENT_LIST_DIR}/fixtures/corpus"
)
add_test(NAME galactic_replay_corpus_contracts
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_replay_corpus.py"
        --program "$<TARGET_FILE:sim_headless>"
        --corpus "${CMAKE_CURRENT_LIST_DIR}/fixtures/corpus"
)

add_test(NAME gc_bench_worker_equality COMMAND gc_bench --ticks 60 --warmup 0)
add_test(NAME gc_bench_work_budgets COMMAND gc_bench --budgets --ticks 60 --warmup 0 --workers 1,2,4,8)

add_executable(galactic_route_tests galactic_route_tests.cpp
    "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/route_lines.cpp")
target_include_directories(galactic_route_tests PRIVATE "${PROJECT_SOURCE_DIR}/apps/viewer/src")
target_link_libraries(galactic_route_tests PRIVATE eawr::presentation)
target_compile_features(galactic_route_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_route_tests)
add_test(NAME galactic_route_contracts COMMAND galactic_route_tests)
eawr_add_python_contract(python_presentation_renderer_galaxy_routes_runtime tests.presentation.renderer.test_viewer_galaxy_routes_runtime)
set_tests_properties(python_presentation_renderer_galaxy_routes_runtime PROPERTIES LABELS gpu)

# The Python suite runs its source checks in root CTest and its graphical
# cases only when opted in on a GPU host.
set_tests_properties(galactic_viewer_mode_contracts PROPERTIES LABELS gpu)

add_executable(galactic_conflict_entry_tests conflict_entry_tests.cpp)
target_link_libraries(galactic_conflict_entry_tests PRIVATE eawr::campaign eawr::platform eawr::scene eawr::checkpoint)
target_compile_features(galactic_conflict_entry_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_conflict_entry_tests)
add_test(NAME galactic_conflict_entry_contracts COMMAND galactic_conflict_entry_tests)
add_test(NAME galactic_map_object_admission COMMAND galactic_conflict_entry_tests --map-admission)
add_test(NAME galactic_handoff_replay_corpus COMMAND galactic_conflict_entry_tests --handoff)
add_test(NAME galactic_conflict_entry_cli
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/conflict_entry_cli_check.py"
        --builder "$<TARGET_FILE:galactic_conflict_entry_tests>"
        --program "$<TARGET_FILE:sim_headless>")
set_tests_properties(galactic_conflict_entry_cli PROPERTIES SKIP_RETURN_CODE 77)

add_executable(galactic_galaxy_graph_tests galaxy_graph_tests.cpp)
target_link_libraries(galactic_galaxy_graph_tests PRIVATE eawr::campaign eawr::checkpoint eawr::platform)
target_compile_features(galactic_galaxy_graph_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_galaxy_graph_tests)
add_test(NAME galactic_galaxy_graph_contracts COMMAND galactic_galaxy_graph_tests)
add_test(NAME galactic_galaxy_graph_stock COMMAND galactic_galaxy_graph_tests --installed)
set_tests_properties(galactic_galaxy_graph_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME galactic_graph_replay_corpus
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/replay_corpus_check.py"
        --program "$<TARGET_FILE:sim_headless>"
        --corpus "${CMAKE_CURRENT_LIST_DIR}/fixtures/graph-corpus")

add_executable(galactic_fleet_tests fleet_tests.cpp)
target_link_libraries(galactic_fleet_tests PRIVATE eawr::campaign eawr::checkpoint eawr::platform)
target_compile_features(galactic_fleet_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_fleet_tests)
add_test(NAME galactic_fleet_contracts COMMAND galactic_fleet_tests)

add_executable(galactic_campaign_setup_tests campaign_setup_tests.cpp)
target_link_libraries(galactic_campaign_setup_tests PRIVATE eawr::campaign eawr::checkpoint eawr::platform)
target_compile_features(galactic_campaign_setup_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_campaign_setup_tests)
add_test(NAME galactic_campaign_setup_contracts COMMAND galactic_campaign_setup_tests)
add_executable(galactic_starting_forces_tests starting_forces_tests.cpp)
target_link_libraries(galactic_starting_forces_tests PRIVATE eawr::campaign eawr::checkpoint eawr::platform)
target_compile_features(galactic_starting_forces_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_starting_forces_tests)
add_test(NAME galactic_starting_forces_contracts COMMAND galactic_starting_forces_tests)
add_test(NAME galactic_campaign_setup_stock COMMAND galactic_campaign_setup_tests --installed)
set_tests_properties(galactic_campaign_setup_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")

add_test(NAME galactic_fleet_replay_corpus
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/replay_corpus_check.py"
        --program "$<TARGET_FILE:sim_headless>"
        --corpus "${CMAKE_CURRENT_LIST_DIR}/fixtures/fleet-corpus")

add_executable(galactic_reference_pages_tests reference_pages_tests.cpp
    "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/reference_catalog.cpp"
    "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/reference_pages.cpp"
    "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/reference_publication.cpp")
target_include_directories(galactic_reference_pages_tests PRIVATE "${PROJECT_SOURCE_DIR}/apps/viewer/src")
target_link_libraries(galactic_reference_pages_tests PRIVATE eawr::sim_galactic eawr::data eawr::vfs)
target_compile_features(galactic_reference_pages_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_reference_pages_tests)
add_test(NAME galactic_reference_pages_contracts COMMAND galactic_reference_pages_tests)
eawr_add_python_contract(python_presentation_renderer_galactic_reference_pages_runtime
    tests.presentation.renderer.test_viewer_galactic_reference_pages_runtime)
set_tests_properties(python_presentation_renderer_galactic_reference_pages_runtime PROPERTIES LABELS gpu)

add_executable(galactic_planet_hud_tests planet_hud_tests.cpp
    "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/planet_hud.cpp")
target_include_directories(galactic_planet_hud_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/apps/viewer/src")
target_compile_features(galactic_planet_hud_tests PRIVATE cxx_std_20)
target_link_libraries(galactic_planet_hud_tests PRIVATE eawr::sim_galactic)
eawr_target_project_warnings(galactic_planet_hud_tests)
add_test(NAME galactic_planet_hud_contracts COMMAND galactic_planet_hud_tests)
add_executable(galactic_fleet_stacks_tests fleet_stacks_tests.cpp)
target_include_directories(galactic_fleet_stacks_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/apps/viewer/src")
target_link_libraries(galactic_fleet_stacks_tests PRIVATE eawr::sim_galactic)
target_compile_features(galactic_fleet_stacks_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_fleet_stacks_tests)
add_test(NAME galactic_fleet_stacks_contracts COMMAND galactic_fleet_stacks_tests)
eawr_add_python_contract(python_presentation_renderer_planet_hud_runtime tests.presentation.renderer.test_viewer_planet_hud_runtime)
set_tests_properties(python_presentation_renderer_planet_hud_runtime PROPERTIES LABELS gpu)
eawr_add_python_contract(python_presentation_renderer_galactic_hud_perf_runtime tests.presentation.renderer.test_viewer_galactic_hud_perf_runtime)
set_tests_properties(python_presentation_renderer_galactic_hud_perf_runtime PROPERTIES LABELS gpu)

eawr_add_python_contract(python_presentation_renderer_galactic_command_bar_runtime tests.presentation.renderer.test_viewer_galactic_command_bar_runtime)
set_tests_properties(python_presentation_renderer_galactic_command_bar_runtime PROPERTIES LABELS gpu)

eawr_add_python_contract(python_presentation_renderer_galactic_production_runtime tests.presentation.renderer.test_viewer_galactic_production_runtime)
set_tests_properties(python_presentation_renderer_galactic_production_runtime PROPERTIES LABELS gpu)

eawr_add_python_contract(python_presentation_renderer_galactic_playtest_runtime tests.presentation.renderer.test_viewer_galactic_playtest_runtime)
set_tests_properties(python_presentation_renderer_galactic_playtest_runtime PROPERTIES LABELS gpu)

eawr_add_python_contract(python_presentation_renderer_galactic_intelligence_runtime tests.presentation.renderer.test_viewer_galactic_intelligence_runtime)
set_tests_properties(python_presentation_renderer_galactic_intelligence_runtime PROPERTIES LABELS gpu)

add_executable(galactic_clock_input_tests clock_input_tests.cpp)
target_include_directories(galactic_clock_input_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/apps/viewer/src")
target_compile_features(galactic_clock_input_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_clock_input_tests)
add_test(NAME galactic_clock_input_contracts COMMAND galactic_clock_input_tests)

add_executable(galactic_campaign_economy_tests campaign_economy_tests.cpp)
target_link_libraries(galactic_campaign_economy_tests PRIVATE eawr::campaign eawr::checkpoint eawr::platform)
target_compile_features(galactic_campaign_economy_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_campaign_economy_tests)
add_test(NAME galactic_campaign_economy_contracts COMMAND galactic_campaign_economy_tests "${CMAKE_CURRENT_LIST_DIR}/fixtures")

add_executable(galactic_production_tests production_tests.cpp)
target_link_libraries(galactic_production_tests PRIVATE eawr::checkpoint eawr::platform)
target_compile_features(galactic_production_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_production_tests)
add_test(NAME galactic_production_contracts COMMAND galactic_production_tests "${CMAKE_CURRENT_LIST_DIR}/fixtures")
add_test(NAME galactic_production_offer_contracts COMMAND galactic_production_tests --offer-projection)
add_test(NAME galactic_retained_census_contracts COMMAND galactic_production_tests --retained-census)
add_test(NAME galactic_ground_sale_contracts COMMAND galactic_production_tests --ground-sale)
add_test(NAME galactic_production_replay_corpus
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/replay_corpus_check.py"
        --program "$<TARGET_FILE:sim_headless>"
        --corpus "${CMAKE_CURRENT_LIST_DIR}/fixtures/production-corpus")
add_executable(galactic_seat_roles_tests seat_roles_tests.cpp)
target_link_libraries(galactic_seat_roles_tests PRIVATE eawr::campaign eawr::checkpoint eawr::platform)
target_compile_features(galactic_seat_roles_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_seat_roles_tests)
add_test(NAME galactic_seat_roles_contracts COMMAND galactic_seat_roles_tests)
add_executable(galactic_movement_tests movement_tests.cpp)
target_link_libraries(galactic_movement_tests PRIVATE eawr::campaign eawr::checkpoint eawr::platform)
target_compile_features(galactic_movement_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_movement_tests)
add_test(NAME galactic_movement_contracts COMMAND galactic_movement_tests)
add_test(NAME galactic_movement_stock COMMAND galactic_movement_tests --installed)
set_tests_properties(galactic_movement_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME galactic_movement_replay_corpus
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/replay_corpus_check.py"
        --program "$<TARGET_FILE:sim_headless>"
        --corpus "${CMAKE_CURRENT_LIST_DIR}/fixtures/movement-corpus")


add_executable(galactic_fiscal_tests fiscal_tests.cpp)
target_link_libraries(galactic_fiscal_tests PRIVATE eawr::checkpoint eawr::platform)
target_compile_features(galactic_fiscal_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_fiscal_tests)
add_test(NAME galactic_fiscal_contracts COMMAND galactic_fiscal_tests)

add_executable(galactic_control_tests control_tests.cpp)
target_sources(galactic_control_tests PRIVATE "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/planet_hud.cpp")
target_include_directories(galactic_control_tests PRIVATE "${PROJECT_SOURCE_DIR}/apps/viewer/src")
target_link_libraries(galactic_control_tests PRIVATE eawr::campaign eawr::checkpoint eawr::platform)
target_compile_features(galactic_control_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_control_tests)
add_test(NAME galactic_control_contracts COMMAND galactic_control_tests)
add_test(NAME galactic_control_stock COMMAND galactic_control_tests --installed)
set_tests_properties(galactic_control_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME galactic_control_replay_corpus
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/replay_corpus_check.py"
        --program "$<TARGET_FILE:sim_headless>"
        --corpus "${CMAKE_CURRENT_LIST_DIR}/fixtures/control-corpus")

# #2880: the seat's surface launch (galactic opcode 18) over the shared native launch owner.
add_executable(galactic_surface_launch_tests surface_launch_tests.cpp)
target_link_libraries(galactic_surface_launch_tests PRIVATE eawr::campaign eawr::checkpoint eawr::platform)
target_compile_features(galactic_surface_launch_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_surface_launch_tests)
add_test(NAME galactic_surface_launch_contracts COMMAND galactic_surface_launch_tests)

eawr_label_tests()

add_executable(galactic_victory_tests victory_tests.cpp)
target_link_libraries(galactic_victory_tests PRIVATE eawr::campaign eawr::platform eawr::checkpoint)
target_compile_features(galactic_victory_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_victory_tests)
add_test(NAME galactic_victory_contracts COMMAND galactic_victory_tests)

eawr_add_python_contract(python_presentation_renderer_galactic_economy_runtime tests.presentation.renderer.test_viewer_galactic_economy_runtime)
set_tests_properties(python_presentation_renderer_galactic_economy_runtime PROPERTIES LABELS gpu)
eawr_add_python_contract(python_presentation_renderer_galactic_planet_block_runtime tests.presentation.renderer.test_viewer_galactic_planet_block_runtime)
set_tests_properties(python_presentation_renderer_galactic_planet_block_runtime PROPERTIES LABELS gpu)
eawr_add_python_contract(python_presentation_renderer_galactic_build_hammer_runtime tests.presentation.renderer.test_viewer_galactic_build_hammer_runtime)
set_tests_properties(python_presentation_renderer_galactic_build_hammer_runtime PROPERTIES LABELS gpu)

eawr_add_python_contract(python_presentation_renderer_galactic_encyclopedia_runtime tests.presentation.renderer.test_viewer_galactic_encyclopedia_runtime)
set_tests_properties(python_presentation_renderer_galactic_encyclopedia_runtime PROPERTIES LABELS gpu)
target_sources(foc_tag_perturb PRIVATE foc_tag_perturb_gc.cpp)
target_include_directories(foc_tag_perturb PRIVATE "${PROJECT_SOURCE_DIR}/tests/skirmish")
target_link_libraries(foc_tag_perturb PRIVATE eawr::campaign)

add_executable(galactic_battle_dialog_tests battle_dialog_tests.cpp)
target_include_directories(galactic_battle_dialog_tests PRIVATE "${PROJECT_SOURCE_DIR}/apps/viewer/src")
target_link_libraries(galactic_battle_dialog_tests PRIVATE eawr::campaign eawr::presentation)
target_compile_features(galactic_battle_dialog_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_battle_dialog_tests)
add_test(NAME galactic_battle_dialog_contracts COMMAND galactic_battle_dialog_tests)
eawr_add_python_contract(python_presentation_renderer_galactic_battle_dialog_runtime tests.presentation.renderer.test_viewer_galactic_battle_dialog_runtime)
set_tests_properties(python_presentation_renderer_galactic_battle_dialog_runtime PROPERTIES LABELS gpu)
eawr_add_python_contract(python_presentation_renderer_galactic_campaign_score_runtime tests.presentation.renderer.test_viewer_galactic_campaign_score_runtime)
set_tests_properties(python_presentation_renderer_galactic_campaign_score_runtime PROPERTIES LABELS gpu)
add_executable(galactic_auto_resolve_live_tests auto_resolve_live_tests.cpp)
target_link_libraries(galactic_auto_resolve_live_tests PRIVATE eawr::campaign eawr::platform eawr::checkpoint)
target_compile_features(galactic_auto_resolve_live_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_auto_resolve_live_tests)
add_test(NAME galactic_auto_resolve_live COMMAND galactic_auto_resolve_live_tests)
add_test(NAME galactic_auto_resolve_live_stock COMMAND galactic_auto_resolve_live_tests --installed)
set_tests_properties(galactic_auto_resolve_live_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_executable(galactic_capture_return_tests capture_return_tests.cpp)
target_link_libraries(galactic_capture_return_tests PRIVATE eawr::campaign eawr::platform eawr::checkpoint)
target_compile_features(galactic_capture_return_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_capture_return_tests)
add_test(NAME galactic_capture_return_stock COMMAND galactic_capture_return_tests)
# The 18 installed invasions include 1,500-frame holds and 1/2/4/8 worker equality.
# This matrix already measured 1,341 seconds before the build-list checks; allow margin on busy hosts.
set_tests_properties(galactic_capture_return_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:" TIMEOUT 3600)
eawr_add_game_data_test(NAME galactic_capture_build_list_empire_stock COMMAND galactic_capture_return_tests --build-list Empire)
add_test(NAME galactic_capture_build_list_rebel_stock COMMAND galactic_capture_return_tests --build-list Rebel)
add_test(NAME galactic_capture_build_list_renamed_stock COMMAND galactic_capture_return_tests --build-list RenamedBuilder)
add_test(NAME galactic_capture_build_list_synthetic COMMAND galactic_control_tests --capture-build-list)
set_tests_properties(galactic_capture_build_list_empire_stock galactic_capture_build_list_rebel_stock galactic_capture_build_list_renamed_stock
    PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:" LABELS game-data)
eawr_add_python_contract(python_presentation_renderer_capture_runtime tests.presentation.renderer.test_viewer_capture_runtime)
set_tests_properties(python_presentation_renderer_capture_runtime PROPERTIES LABELS gpu)

add_executable(galactic_live_battle_dialog_tests live_battle_dialog_tests.cpp)
target_include_directories(galactic_live_battle_dialog_tests PRIVATE "${PROJECT_SOURCE_DIR}/apps/viewer/src")
target_link_libraries(galactic_live_battle_dialog_tests PRIVATE eawr::campaign eawr::platform eawr::presentation)
target_compile_features(galactic_live_battle_dialog_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_live_battle_dialog_tests)
add_test(NAME galactic_live_battle_dialog COMMAND galactic_live_battle_dialog_tests)
# GC #4030: installed live campaign with the native AI; the local seat's battle dialog opens only for its own conflicts.
add_executable(galactic_uninvolved_conflict_ui_tests uninvolved_conflict_ui_stock_tests.cpp)
target_include_directories(galactic_uninvolved_conflict_ui_tests PRIVATE "${PROJECT_SOURCE_DIR}/apps/viewer/src")
target_link_libraries(galactic_uninvolved_conflict_ui_tests PRIVATE eawr::campaign eawr::platform eawr::presentation)
target_compile_features(galactic_uninvolved_conflict_ui_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_uninvolved_conflict_ui_tests)
eawr_add_game_data_test(NAME galactic_uninvolved_conflict_ui_stock COMMAND galactic_uninvolved_conflict_ui_tests 12500)
set_tests_properties(galactic_uninvolved_conflict_ui_stock PROPERTIES LABELS game-data TIMEOUT 7200 RUN_SERIAL TRUE
    ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")

add_executable(galactic_invasion_publication_tests invasion_publication_tests.cpp)
target_link_libraries(galactic_invasion_publication_tests PRIVATE eawr::campaign eawr::platform)
target_compile_features(galactic_invasion_publication_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_invasion_publication_tests)
add_test(NAME galactic_invasion_publication COMMAND galactic_invasion_publication_tests)
eawr_add_python_contract(python_presentation_renderer_live_launch_slot_roster tests.presentation.renderer.test_live_launch_slot_roster)
eawr_add_python_contract(python_presentation_renderer_gc_space_cycle tests.presentation.renderer.test_viewer_gc_space_cycle)
set_tests_properties(python_presentation_renderer_gc_space_cycle PROPERTIES LABELS gpu)
add_executable(galactic_space_entry_tests space_entry_tests.cpp)
target_link_libraries(galactic_space_entry_tests PRIVATE eawr::campaign eawr::platform eawr::checkpoint)
target_compile_features(galactic_space_entry_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_space_entry_tests)
add_test(NAME galactic_space_entry COMMAND galactic_space_entry_tests)
add_test(NAME galactic_space_entry_stock COMMAND galactic_space_entry_tests --installed)
add_test(NAME galactic_space_commander_stock COMMAND galactic_space_entry_tests --installed-commanders)
set_tests_properties(galactic_space_commander_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME galactic_space_population_stock COMMAND galactic_space_entry_tests --installed-space-population)
set_tests_properties(galactic_space_population_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
set_tests_properties(galactic_space_entry_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME galactic_space_garrison_stock COMMAND galactic_space_entry_tests --installed-garrison)
add_test(NAME galactic_space_superweapon_stock COMMAND galactic_space_entry_tests --installed-superweapon)
set_tests_properties(galactic_space_superweapon_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
set_tests_properties(galactic_space_garrison_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_executable(galactic_space_rules_tests space_rules_tests.cpp)
target_link_libraries(galactic_space_rules_tests PRIVATE eawr::campaign eawr::platform eawr::checkpoint)
target_compile_features(galactic_space_rules_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_space_rules_tests)
add_test(NAME galactic_space_rules COMMAND galactic_space_rules_tests)
add_test(NAME galactic_space_rules_stock COMMAND galactic_space_rules_tests --installed)
set_tests_properties(galactic_space_rules_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")


# WST-30..45/52 fixture story boundary, independent of live session composition.
add_executable(galactic_story_adapters_tests story_adapters_tests.cpp)
target_link_libraries(galactic_story_adapters_tests PRIVATE eawr::sim_galactic eawr::data eawr::vfs)
target_compile_features(galactic_story_adapters_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_story_adapters_tests)
add_test(NAME galactic_story_adapters_contracts COMMAND galactic_story_adapters_tests)
add_test(NAME galactic_story_adapters_stock COMMAND galactic_story_adapters_tests --installed)
set_tests_properties(galactic_story_adapters_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
# WGA-11..24/53..54 fixture-only category/goal funding ledger.
add_executable(galactic_ai_funding_tests ai_funding_tests.cpp)
target_link_libraries(galactic_ai_funding_tests PRIVATE eawr::sim_galactic)
target_compile_features(galactic_ai_funding_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_funding_tests)
add_test(NAME galactic_ai_funding_contracts COMMAND galactic_ai_funding_tests)
# WST-47/58 fixture goals and presentation sink.
add_executable(galactic_story_goals_tests story_goals_tests.cpp)
target_link_libraries(galactic_story_goals_tests PRIVATE eawr::sim_galactic)
target_compile_features(galactic_story_goals_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_story_goals_tests)
add_test(NAME galactic_story_goals_contracts COMMAND galactic_story_goals_tests)

# WGA-57..60 supporting component contracts; installed stock proof stays open.
add_executable(galactic_ai_plans_tests ai_plans_stock_tests.cpp)
target_include_directories(galactic_ai_plans_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc")
target_link_libraries(galactic_ai_plans_tests PRIVATE eawr::script_foc eawr::platform)
target_compile_features(galactic_ai_plans_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_plans_tests)
add_test(NAME galactic_ai_plans_contracts COMMAND galactic_ai_plans_tests)

# Definition/data proof; strict installed mode requires the verified game mirror.
add_executable(galactic_ai_plan_loading_tests ai_plan_loading_stock_tests.cpp ai_goal_state_stock_tests.cpp ai_learning_tests.cpp)
target_include_directories(galactic_ai_plan_loading_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc")
target_link_libraries(galactic_ai_plan_loading_tests PRIVATE eawr::script_foc eawr::vfs eawr::campaign eawr::platform)
target_compile_features(galactic_ai_plan_loading_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_plan_loading_tests)
add_test(NAME galactic_ai_plan_loading_contracts COMMAND galactic_ai_plan_loading_tests)
eawr_add_game_data_test(NAME galactic_ai_plan_loading_stock COMMAND galactic_ai_plan_loading_tests --installed)
set_tests_properties(galactic_ai_plan_loading_stock PROPERTIES LABELS game-data)
# Supporting live ledger hooks; installed stock-plan acceptance is composed by S4.
add_executable(galactic_ai_funding_live_tests ai_funding_stock_tests.cpp)
target_link_libraries(galactic_ai_funding_live_tests PRIVATE eawr::script_foc eawr::platform)
target_compile_features(galactic_ai_funding_live_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_funding_live_tests)
add_test(NAME galactic_ai_funding_live_contracts COMMAND galactic_ai_funding_live_tests)

# Six-owner composition is infrastructure; stock runtime/victory remains separate.
add_executable(galactic_ai_campaign_tests ai_campaign_stock_tests.cpp)
target_include_directories(galactic_ai_campaign_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/script/foc")
target_link_libraries(galactic_ai_campaign_tests PRIVATE eawr::checkpoint eawr::platform)
target_compile_features(galactic_ai_campaign_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_campaign_tests)
add_test(NAME galactic_ai_campaign_runtime_contracts COMMAND galactic_ai_campaign_tests)
add_executable(galactic_ai_profile_tests gc_profile_tests.cpp)
target_include_directories(galactic_ai_profile_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(galactic_ai_profile_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_profile_tests)
add_test(NAME galactic_ai_profile_contracts COMMAND galactic_ai_profile_tests)
if(WIN32)
  eawr_add_game_data_test(NAME galactic_ai_profile_retail600 COMMAND galactic_ai_campaign_tests --live-profile-comparison
    "${CMAKE_CURRENT_BINARY_DIR}/live-profile" 600)
  set_tests_properties(galactic_ai_profile_retail600 PROPERTIES LABELS "game-data"
    TIMEOUT 14400 RUN_SERIAL TRUE ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
endif()
eawr_add_game_data_test(NAME galactic_ai_campaign_preparation_stock COMMAND galactic_ai_campaign_tests --installed)
eawr_add_game_data_test(NAME galactic_ai_live_production_stock COMMAND galactic_ai_campaign_tests --live-production
    "${CMAKE_CURRENT_BINARY_DIR}/live-production" 1800)
eawr_add_game_data_test(NAME galactic_ai_live_performance_stock COMMAND galactic_ai_campaign_tests --live-performance
    "${CMAKE_CURRENT_BINARY_DIR}/live-performance" 30)
set_tests_properties(galactic_ai_live_production_stock galactic_ai_live_performance_stock PROPERTIES
    LABELS "game-data" TIMEOUT 28800 RUN_SERIAL TRUE ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
set_tests_properties(galactic_ai_campaign_runtime_contracts PROPERTIES ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
set_tests_properties(galactic_ai_campaign_preparation_stock PROPERTIES LABELS game-data ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
# WGA-436..439: installed stock free-store units actually move (movement counter, in-transit witness,
# no unbound refusal) with equal state hashes at every determinism worker count.
eawr_add_game_data_test(NAME galactic_ai_freestore_movement_stock COMMAND galactic_ai_campaign_tests --freestore-movement
    "${CMAKE_CURRENT_BINARY_DIR}/freestore-movement" 1800)
set_tests_properties(galactic_ai_freestore_movement_stock PROPERTIES LABELS game-data TIMEOUT 7200 RUN_SERIAL TRUE
    ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
eawr_add_game_data_test(NAME galactic_ai_campaign_release_stock COMMAND galactic_ai_campaign_tests --release "${CMAKE_CURRENT_BINARY_DIR}/campaign-gate-correctness")
eawr_add_game_data_test(NAME galactic_ai_campaign_digest_mature_stock COMMAND galactic_ai_campaign_tests --release-benchmark "${CMAKE_CURRENT_BINARY_DIR}/campaign-gate-benchmark")
set_tests_properties(galactic_ai_campaign_release_stock PROPERTIES LABELS "game-data;release-gate" ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
set_tests_properties(galactic_ai_campaign_digest_mature_stock PROPERTIES LABELS "game-data;release-gate;benchmark" ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
set_tests_properties(galactic_ai_campaign_release_stock galactic_ai_campaign_digest_mature_stock PROPERTIES TIMEOUT 7200 RUN_SERIAL TRUE)
# The 27,000-frame correctness gate can take about 2.5 hours with full hash
# verification; allow four hours without changing its assertions or frame limit.
set_tests_properties(galactic_ai_campaign_release_stock PROPERTIES TIMEOUT 14400)
eawr_add_game_data_test(NAME galactic_ai_campaign_policy_equality_stock COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/galactic/ai_campaign_policy_compare.py"
    "${CMAKE_CURRENT_BINARY_DIR}/campaign-gate-correctness" "${CMAKE_CURRENT_BINARY_DIR}/campaign-gate-benchmark")
set_tests_properties(galactic_ai_campaign_policy_equality_stock PROPERTIES LABELS "game-data;release-gate"
    SKIP_REGULAR_EXPRESSION "SKIPPED:"
    DEPENDS "galactic_ai_campaign_release_stock;galactic_ai_campaign_digest_mature_stock")
# --release is deliberately not a passing CTest registration while its actual
# gameplay/listener providers remain absent; the release owner runs it explicitly.
add_executable(galactic_ai_composed_state_tests ai_composed_state_tests.cpp)
target_include_directories(galactic_ai_composed_state_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/script/foc")
target_link_libraries(galactic_ai_composed_state_tests PRIVATE eawr::checkpoint eawr::platform)
target_compile_features(galactic_ai_composed_state_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_composed_state_tests)
add_test(NAME galactic_ai_composed_state_contracts COMMAND galactic_ai_composed_state_tests)
eawr_add_game_data_test(NAME galactic_ai_composed_state_stock COMMAND galactic_ai_composed_state_tests --installed)
set_tests_properties(galactic_ai_composed_state_contracts PROPERTIES ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
set_tests_properties(galactic_ai_composed_state_stock PROPERTIES LABELS game-data ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
# The same frozen stock state measures the full-capture baseline and the Release
# incremental path. Correctness cases above keep verification on every request.
eawr_add_game_data_test(NAME galactic_ai_digest_benchmark_stock COMMAND galactic_ai_composed_state_tests --benchmark)
set_tests_properties(galactic_ai_digest_benchmark_stock PROPERTIES LABELS game-data ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")

# WGA-01..05/GFD-A04: lane A shared copied-input and capability contract.
add_executable(galactic_ai_selector_runtime_tests ai_selector_runtime_tests.cpp ai_synchronized_staging_tests.cpp)
target_include_directories(galactic_ai_selector_runtime_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc")
target_link_libraries(galactic_ai_selector_runtime_tests PRIVATE eawr::script_foc eawr::platform)
target_compile_features(galactic_ai_selector_runtime_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_selector_runtime_tests)
add_test(NAME galactic_ai_selector_runtime_contracts COMMAND galactic_ai_selector_runtime_tests)

add_executable(galactic_ai_frontier_runtime_tests ai_frontier_runtime_tests.cpp)
target_include_directories(galactic_ai_frontier_runtime_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc")
target_link_libraries(galactic_ai_frontier_runtime_tests PRIVATE eawr::checkpoint eawr::platform)
target_compile_features(galactic_ai_frontier_runtime_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_frontier_runtime_tests)
eawr_add_game_data_test(NAME galactic_ai_frontier_stock COMMAND galactic_ai_frontier_runtime_tests)
set_tests_properties(galactic_ai_frontier_stock PROPERTIES LABELS game-data ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0" TIMEOUT 3600)

# #3903: single-record perception encoders equal the whole-state encoder and allocate nothing warm.
add_executable(galactic_ai_perception_encoding_tests ai_perception_encoding_tests.cpp)
target_include_directories(galactic_ai_perception_encoding_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc")
target_link_libraries(galactic_ai_perception_encoding_tests PRIVATE eawr::script_foc eawr::platform)
target_compile_features(galactic_ai_perception_encoding_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_perception_encoding_tests)
add_test(NAME galactic_ai_perception_encoding_contracts COMMAND galactic_ai_perception_encoding_tests)

add_executable(galactic_ai_selector_stock_tests ai_selector_stock_tests.cpp)
target_include_directories(galactic_ai_selector_stock_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc")
target_link_libraries(galactic_ai_selector_stock_tests PRIVATE eawr::script_foc eawr::campaign eawr::platform)
target_compile_features(galactic_ai_selector_stock_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_selector_stock_tests)
eawr_add_game_data_test(NAME galactic_ai_selector_stock COMMAND galactic_ai_selector_stock_tests)
set_tests_properties(galactic_ai_selector_stock PROPERTIES LABELS game-data ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
eawr_add_game_data_test(NAME galactic_ai_selector_normal_stock COMMAND galactic_ai_selector_stock_tests --normal)
set_tests_properties(galactic_ai_selector_normal_stock PROPERTIES LABELS game-data ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
set_tests_properties(galactic_ai_selector_runtime_contracts PROPERTIES ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")

add_executable(galactic_ai_economic_replay_tests ai_economic_replay_tests.cpp)
target_include_directories(galactic_ai_economic_replay_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc")
target_link_libraries(galactic_ai_economic_replay_tests PRIVATE eawr::script_foc eawr::platform)
target_compile_features(galactic_ai_economic_replay_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_economic_replay_tests)
add_test(NAME galactic_ai_economic_replay_contracts COMMAND galactic_ai_economic_replay_tests)

# WGA-56/139: F native checkpoint/live disk boundary contracts, no stock proof.
add_executable(galactic_ai_checkpoint_tests ai_checkpoint_stock_tests.cpp)
target_link_libraries(galactic_ai_checkpoint_tests PRIVATE eawr::checkpoint eawr::platform)
target_compile_features(galactic_ai_checkpoint_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_checkpoint_tests)
add_test(NAME galactic_ai_checkpoint_contracts COMMAND galactic_ai_checkpoint_tests "${CMAKE_CURRENT_BINARY_DIR}/native-ai-saves")

add_executable(galactic_ai_interface_tests ai_interface_tests.cpp)
target_link_libraries(galactic_ai_interface_tests PRIVATE eawr::script_authoritative eawr::sim_galactic)
target_compile_features(galactic_ai_interface_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_interface_tests)
add_test(NAME galactic_ai_interface_contracts COMMAND galactic_ai_interface_tests)

# WGA-01..05/09/102..110: supporting lifecycle fixtures and real stock loading.
# The composed live-opponent primary proof remains a separate integration seam.
add_executable(galactic_ai_lifecycle_tests ai_lifecycle_stock_tests.cpp)
target_include_directories(galactic_ai_lifecycle_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc")
target_link_libraries(galactic_ai_lifecycle_tests PRIVATE eawr::script_foc eawr::script_galactic eawr::platform eawr::vfs)
target_compile_features(galactic_ai_lifecycle_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_lifecycle_tests)
add_test(NAME galactic_ai_lifecycle_contracts COMMAND galactic_ai_lifecycle_tests)
eawr_add_game_data_test(NAME galactic_ai_lifecycle_loading_stock COMMAND galactic_ai_lifecycle_tests --installed)
set_tests_properties(galactic_ai_lifecycle_loading_stock PROPERTIES LABELS game-data)
add_test(NAME galactic_ai_lifecycle_state_contracts COMMAND galactic_ai_lifecycle_tests --state)
foreach(workers IN ITEMS 1 2 4 8)
    add_test(NAME galactic_ai_bounded_diagnostics_${workers} COMMAND galactic_ai_lifecycle_tests --bounded ${workers})
endforeach()
# Shared immutable per-player policy snapshots: zero copies while unchanged, one per write/restore.
add_executable(galactic_ai_policy_snapshot_tests ai_policy_snapshot_tests.cpp)
target_include_directories(galactic_ai_policy_snapshot_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc")
target_link_libraries(galactic_ai_policy_snapshot_tests PRIVATE eawr::script_foc eawr::script_galactic eawr::platform)
target_compile_features(galactic_ai_policy_snapshot_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_policy_snapshot_tests)
add_test(NAME galactic_ai_policy_snapshot_contracts COMMAND galactic_ai_policy_snapshot_tests)
# WGA-27..32/61/77/78: execution route publication and stock freestore admission.
add_executable(galactic_ai_execution_tests ai_execution_stock_tests.cpp)
target_include_directories(galactic_ai_execution_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc")
target_link_libraries(galactic_ai_execution_tests PRIVATE eawr::script_foc eawr::campaign eawr::platform)
target_compile_features(galactic_ai_execution_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_execution_tests)
add_test(NAME galactic_ai_execution_contracts COMMAND galactic_ai_execution_tests)
eawr_add_game_data_test(NAME galactic_ai_execution_stock COMMAND galactic_ai_execution_tests --installed)
set_tests_properties(galactic_ai_execution_stock PROPERTIES LABELS game-data)

# WGA-227..231/234: direct fleet admission, ordinary hop recomputation and native continuation.
add_executable(galactic_ai_freestore_tests ai_freestore_tests.cpp)
target_include_directories(galactic_ai_freestore_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc")
target_link_libraries(galactic_ai_freestore_tests PRIVATE eawr::script_foc eawr::campaign eawr::checkpoint eawr::platform)
target_compile_features(galactic_ai_freestore_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_freestore_tests)
add_test(NAME galactic_ai_freestore_contracts COMMAND galactic_ai_freestore_tests)
eawr_add_game_data_test(NAME galactic_ai_freestore_stock COMMAND galactic_ai_freestore_tests --installed)
set_tests_properties(galactic_ai_freestore_contracts PROPERTIES ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
set_tests_properties(galactic_ai_freestore_stock PROPERTIES LABELS game-data ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")

add_executable(galactic_ai_movement_tests ai_movement_tests.cpp)
target_include_directories(galactic_ai_movement_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc")
target_link_libraries(galactic_ai_movement_tests PRIVATE eawr::script_foc eawr::campaign eawr::checkpoint eawr::platform)
target_compile_features(galactic_ai_movement_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_movement_tests)
add_test(NAME galactic_ai_movement_contracts COMMAND galactic_ai_movement_tests)
eawr_add_game_data_test(NAME galactic_ai_movement_stock COMMAND galactic_ai_movement_tests --installed)
set_tests_properties(galactic_ai_movement_stock PROPERTIES LABELS game-data)

# Lane B component proof; lifecycle/actions and retained-route writer compose later.
add_executable(galactic_ai_usability_tests ai_usability_tests.cpp)
target_include_directories(galactic_ai_usability_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc" "${PROJECT_SOURCE_DIR}/src/campaign")
target_link_libraries(galactic_ai_usability_tests PRIVATE eawr::campaign eawr::checkpoint eawr::platform)
target_compile_features(galactic_ai_usability_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_usability_tests)
add_test(NAME galactic_ai_usability_contracts COMMAND galactic_ai_usability_tests)
set_tests_properties(galactic_ai_usability_contracts PROPERTIES ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
add_executable(galactic_ai_usability_stock_tests ai_usability_stock_tests.cpp)
target_include_directories(galactic_ai_usability_stock_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc" "${PROJECT_SOURCE_DIR}/src/campaign")
target_link_libraries(galactic_ai_usability_stock_tests PRIVATE eawr::campaign eawr::checkpoint eawr::platform)
target_compile_features(galactic_ai_usability_stock_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_usability_stock_tests)
eawr_add_game_data_test(NAME galactic_ai_usability_stock COMMAND galactic_ai_usability_stock_tests)
set_tests_properties(galactic_ai_usability_stock PROPERTIES LABELS game-data ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")

add_executable(galactic_ai_perception_stock_tests ai_perception_stock_tests.cpp)
target_include_directories(galactic_ai_perception_stock_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc")
target_link_libraries(galactic_ai_perception_stock_tests PRIVATE eawr::script_foc eawr::campaign eawr::platform)
target_compile_features(galactic_ai_perception_stock_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_perception_stock_tests)
add_test(NAME galactic_ai_perception_contracts COMMAND galactic_ai_perception_stock_tests)
add_test(NAME galactic_ai_perception_stock COMMAND galactic_ai_perception_stock_tests --installed)
set_tests_properties(galactic_ai_perception_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")

add_executable(galactic_ai_economy_tokens_tests ai_economy_tokens_tests.cpp)
target_include_directories(galactic_ai_economy_tokens_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc")
target_link_libraries(galactic_ai_economy_tokens_tests PRIVATE eawr::script_foc eawr::campaign eawr::platform)
target_compile_features(galactic_ai_economy_tokens_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_economy_tokens_tests)
add_test(NAME galactic_ai_economy_tokens_contracts COMMAND galactic_ai_economy_tokens_tests)
set_tests_properties(galactic_ai_economy_tokens_contracts PROPERTIES ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
eawr_add_game_data_test(NAME galactic_ai_economy_tokens_stock COMMAND galactic_ai_economy_tokens_tests --installed)
set_tests_properties(galactic_ai_economy_tokens_stock PROPERTIES LABELS game-data ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")

add_executable(galactic_corruption_actions_tests corruption_actions_tests.cpp)
target_link_libraries(galactic_corruption_actions_tests PRIVATE eawr::sim_galactic)
target_compile_features(galactic_corruption_actions_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_corruption_actions_tests)
add_test(NAME galactic_corruption_actions_contracts COMMAND galactic_corruption_actions_tests)

add_executable(galactic_corruption_views_tests corruption_views_tests.cpp)
target_link_libraries(galactic_corruption_views_tests PRIVATE eawr::sim_galactic)
target_compile_features(galactic_corruption_views_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_corruption_views_tests)
add_test(NAME galactic_corruption_views_contracts COMMAND galactic_corruption_views_tests)

add_executable(galactic_hero_actions_tests hero_actions_tests.cpp)
target_link_libraries(galactic_hero_actions_tests PRIVATE eawr::sim_galactic)
target_compile_features(galactic_hero_actions_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_hero_actions_tests)
add_test(NAME galactic_hero_actions_contracts COMMAND galactic_hero_actions_tests)

add_executable(galactic_hero_action_views_tests hero_action_views_tests.cpp)
target_link_libraries(galactic_hero_action_views_tests PRIVATE eawr::sim_galactic)
target_compile_features(galactic_hero_action_views_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_hero_action_views_tests)
add_test(NAME galactic_hero_action_views_contracts COMMAND galactic_hero_action_views_tests)

add_executable(galactic_economy_corruption_tests economy_corruption_tests.cpp)
target_link_libraries(galactic_economy_corruption_tests PRIVATE eawr::campaign eawr::checkpoint eawr::platform eawr::vfs)
target_compile_features(galactic_economy_corruption_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_economy_corruption_tests)
add_test(NAME galactic_economy_corruption_contracts COMMAND galactic_economy_corruption_tests)
eawr_add_game_data_test(NAME galactic_economy_corruption_stock COMMAND galactic_economy_corruption_tests --installed)
set_tests_properties(galactic_economy_corruption_contracts PROPERTIES ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
set_tests_properties(galactic_economy_corruption_stock PROPERTIES LABELS game-data ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
eawr_add_python_contract(python_presentation_renderer_galactic_hero_actions_runtime tests.presentation.renderer.test_viewer_galactic_hero_actions_runtime)
add_executable(galactic_ai_markup_tests ai_markup_tests.cpp)
target_include_directories(galactic_ai_markup_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc")
target_link_libraries(galactic_ai_markup_tests PRIVATE eawr::script_foc eawr::platform)
target_compile_features(galactic_ai_markup_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_markup_tests)
add_test(NAME galactic_ai_markup_contracts COMMAND galactic_ai_markup_tests)
add_executable(galactic_ai_markup_stock_tests ai_markup_stock_tests.cpp)
target_include_directories(galactic_ai_markup_stock_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc")
target_link_libraries(galactic_ai_markup_stock_tests PRIVATE eawr::campaign eawr::checkpoint eawr::platform)
target_compile_features(galactic_ai_markup_stock_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_markup_stock_tests)
eawr_add_game_data_test(NAME galactic_ai_markup_stock COMMAND galactic_ai_markup_stock_tests --installed "${CMAKE_CURRENT_BINARY_DIR}/markup-stock")
set_tests_properties(galactic_ai_markup_stock PROPERTIES LABELS game-data TIMEOUT 1800)
eawr_add_game_data_test(NAME galactic_ai_markup_drift_stock COMMAND galactic_ai_markup_stock_tests --drift "${CMAKE_CURRENT_BINARY_DIR}/markup-drift" 27000)
set_tests_properties(galactic_ai_markup_drift_stock PROPERTIES LABELS "game-data;release-gate" TIMEOUT 14400 RUN_SERIAL TRUE)
set_tests_properties(python_presentation_renderer_galactic_hero_actions_runtime PROPERTIES LABELS gpu)

add_executable(galactic_ai_access_tests ai_access_tests.cpp)
target_include_directories(galactic_ai_access_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc")
target_link_libraries(galactic_ai_access_tests PRIVATE eawr::campaign eawr::checkpoint eawr::platform)
target_compile_features(galactic_ai_access_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_access_tests)
add_test(NAME galactic_ai_access_contracts COMMAND galactic_ai_access_tests)
set_tests_properties(galactic_ai_access_contracts PROPERTIES ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")

# S9-early (GC AI redesign): per-player galactic random streams and identity ranges, and the
# partition-order tripwire over the galactic AI barrier. Synthetic fixtures, no game data.
foreach(s9_test IN ITEMS ai_stream_ids ai_partition_order ai_player_state ai_perception_shards)
    add_executable(galactic_${s9_test}_tests ${s9_test}_tests.cpp)
    target_include_directories(galactic_${s9_test}_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc")
    target_link_libraries(galactic_${s9_test}_tests PRIVATE eawr::script_foc eawr::platform)
    target_compile_features(galactic_${s9_test}_tests PRIVATE cxx_std_20)
    eawr_target_project_warnings(galactic_${s9_test}_tests)
    add_test(NAME galactic_${s9_test}_contracts COMMAND galactic_${s9_test}_tests)
endforeach()

# Retail-cadence 600-frame campaign on the installed game: per-tick hashes and committed events equal on
# 1/2/4/8 workers, and a save at the window midpoint restored on a different worker count continues equal.
eawr_add_game_data_test(NAME galactic_ai_s9_retail600_stock COMMAND galactic_ai_campaign_tests --live-cadence
    "${CMAKE_CURRENT_BINARY_DIR}/s9-retail600" 600)
set_tests_properties(galactic_ai_s9_retail600_stock PROPERTIES LABELS game-data TIMEOUT 7200 RUN_SERIAL TRUE
    ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")

# GC AI S3a: the world reader's structural index, fog bits and double-buffered view (synthetic, 1/2/4/8 workers).
add_executable(galactic_ai_world_reader_tests ai_world_reader_tests.cpp)
target_include_directories(galactic_ai_world_reader_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc")
target_link_libraries(galactic_ai_world_reader_tests PRIVATE eawr::script_foc eawr::campaign eawr::platform)
target_compile_features(galactic_ai_world_reader_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_world_reader_tests)
add_test(NAME galactic_ai_world_reader_contracts COMMAND galactic_ai_world_reader_tests)

# WGA-485: bounded stock observation, matching the seed-1 idle-human day gate.
add_executable(galactic_ai_synchronized_day_tests ai_synchronized_day_tests.cpp)
target_include_directories(galactic_ai_synchronized_day_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc")
target_link_libraries(galactic_ai_synchronized_day_tests PRIVATE eawr::checkpoint eawr::platform)
target_compile_features(galactic_ai_synchronized_day_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_synchronized_day_tests)
eawr_add_game_data_test(NAME galactic_ai_synchronized_day_stock COMMAND galactic_ai_synchronized_day_tests)
set_tests_properties(galactic_ai_synchronized_day_stock PROPERTIES LABELS "game-data;observation" TIMEOUT 3600 RUN_SERIAL TRUE
    ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")

# #3944: seeded idle-human day run of the stock Underworld AI (Underworld is an AI seat in Sandbox_Equal_Footing_Empire).
add_executable(galactic_ai_underworld_day_tests ai_underworld_day_tests.cpp)
target_include_directories(galactic_ai_underworld_day_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc")
target_link_libraries(galactic_ai_underworld_day_tests PRIVATE eawr::checkpoint eawr::platform)
target_compile_features(galactic_ai_underworld_day_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_ai_underworld_day_tests)
eawr_add_game_data_test(NAME galactic_ai_underworld_day_stock COMMAND galactic_ai_underworld_day_tests 5400)
eawr_add_game_data_test(NAME galactic_ai_underworld_day_workers COMMAND galactic_ai_underworld_day_tests --equality 1800)
set_tests_properties(galactic_ai_underworld_day_stock galactic_ai_underworld_day_workers PROPERTIES LABELS "game-data;observation"
    TIMEOUT 14400 RUN_SERIAL TRUE ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")

add_executable(galactic_notice_text_tests notice_text_tests.cpp)
target_include_directories(galactic_notice_text_tests PRIVATE "${PROJECT_SOURCE_DIR}/apps/viewer/src" "${PROJECT_SOURCE_DIR}/tests/ui")
target_link_libraries(galactic_notice_text_tests PRIVATE eawr::ui_data)
target_compile_features(galactic_notice_text_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_notice_text_tests)
add_test(NAME galactic_notice_text COMMAND galactic_notice_text_tests)
set_tests_properties(galactic_notice_text PROPERTIES LABELS "game-data" SKIP_REGULAR_EXPRESSION "corpus: skipped")
