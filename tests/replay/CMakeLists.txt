find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

add_executable(replay_contract_tests replay_tests.cpp)
target_link_libraries(replay_contract_tests PRIVATE eawr::sim eawr::platform)
target_compile_features(replay_contract_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(replay_contract_tests)

set(EAWR_REPLAY_FIXTURES "${CMAKE_CURRENT_LIST_DIR}/fixtures")

add_executable(tactical_contract_tests tactical_tests.cpp)
target_link_libraries(tactical_contract_tests PRIVATE eawr::sim eawr::platform eawr::script_authoritative)
target_compile_features(tactical_contract_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(tactical_contract_tests)
add_test(
    NAME tactical_replay_contracts
    COMMAND tactical_contract_tests "${EAWR_REPLAY_FIXTURES}"
)
add_executable(tactical_space_tests space_tests.cpp)
target_link_libraries(tactical_space_tests PRIVATE eawr::sim eawr::platform)
target_compile_features(tactical_space_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(tactical_space_tests)
add_test(
    NAME tactical_space_contracts
    COMMAND tactical_space_tests "${EAWR_REPLAY_FIXTURES}"
)
add_executable(tactical_squadron_fog_tests squadron_fog_tests.cpp)
target_link_libraries(tactical_squadron_fog_tests PRIVATE eawr::sim eawr::platform)
target_compile_features(tactical_squadron_fog_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(tactical_squadron_fog_tests)
add_test(
    NAME tactical_squadron_fog_contracts
    COMMAND tactical_squadron_fog_tests
)
add_executable(tactical_durability_tests durability_tests.cpp)
target_link_libraries(tactical_durability_tests PRIVATE eawr::sim eawr::platform)
target_compile_features(tactical_durability_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(tactical_durability_tests)
add_test(
    NAME tactical_durability_contracts
    COMMAND tactical_durability_tests "${EAWR_REPLAY_FIXTURES}"
)
# #76: unit abilities (DEFEND, TURBO, POWER_TO_WEAPONS, SPOILER_LOCK).
add_executable(tactical_ability_tests ability_tests.cpp)
target_link_libraries(tactical_ability_tests PRIVATE eawr::sim eawr::platform)
target_compile_features(tactical_ability_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(tactical_ability_tests)
add_test(
    NAME tactical_ability_contracts
    COMMAND tactical_ability_tests
)
# #77: fixed-force victory and defeat.
add_executable(tactical_victory_tests victory_tests.cpp)
target_link_libraries(tactical_victory_tests PRIVATE eawr::sim eawr::platform)
target_compile_features(tactical_victory_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(tactical_victory_tests)
add_test(
    NAME tactical_victory_contracts
    COMMAND tactical_victory_tests "${EAWR_REPLAY_FIXTURES}"
)
add_executable(tactical_motion_tests motion_tests.cpp)
target_link_libraries(tactical_motion_tests PRIVATE eawr::sim eawr::platform)
target_compile_features(tactical_motion_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(tactical_motion_tests)
add_test(
    NAME tactical_motion_contracts
    COMMAND tactical_motion_tests "${EAWR_REPLAY_FIXTURES}"
)
# #71: the space path finder and tracking layers.
add_executable(tactical_pathfind_tests pathfind_tests.cpp)
target_link_libraries(tactical_pathfind_tests PRIVATE eawr::sim eawr::platform)
target_compile_features(tactical_pathfind_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(tactical_pathfind_tests)
add_test(
    NAME tactical_pathfind_contracts
    COMMAND tactical_pathfind_tests "${EAWR_REPLAY_FIXTURES}"
)
# #520: the path search's work, pinned per search (counts, not time).
add_executable(tactical_path_cost_tests path_cost_tests.cpp)
target_link_libraries(tactical_path_cost_tests PRIVATE eawr::sim)
target_compile_features(tactical_path_cost_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(tactical_path_cost_tests)
add_test(NAME tactical_path_cost_contracts COMMAND tactical_path_cost_tests "${EAWR_REPLAY_FIXTURES}")
# #344: group moves (FoC's space movement coordinator).
add_executable(tactical_formation_tests formation_tests.cpp)
target_link_libraries(tactical_formation_tests PRIVATE eawr::sim eawr::platform)
target_compile_features(tactical_formation_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(tactical_formation_tests)
add_test(
    NAME tactical_formation_contracts
    COMMAND tactical_formation_tests
)
# #75: fighter squadrons (hangars, the fighter locomotor, squadron targets).
add_executable(tactical_fighter_tests fighter_tests.cpp)
target_link_libraries(tactical_fighter_tests PRIVATE eawr::sim eawr::platform)
target_compile_features(tactical_fighter_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(tactical_fighter_tests)
add_test(
    NAME tactical_fighter_contracts
    COMMAND tactical_fighter_tests
)
# #447: spin-away deaths of fighters.
add_executable(tactical_fighter_death_tests fighter_death_tests.cpp)
target_link_libraries(tactical_fighter_death_tests PRIVATE eawr::sim eawr::platform)
target_compile_features(tactical_fighter_death_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(tactical_fighter_death_tests)
add_test(
    NAME tactical_fighter_death_contracts
    COMMAND tactical_fighter_death_tests
)
# #479: how a squadron flies (space-fighters C-18 to C-21: no slip, banking, the loop, formation bank).
add_executable(tactical_flight_tests flight_tests.cpp)
target_link_libraries(tactical_flight_tests PRIVATE eawr::sim)
target_compile_features(tactical_flight_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(tactical_flight_tests)
add_test(
    NAME tactical_flight_contracts
    COMMAND tactical_flight_tests
)
# #457: squadron dogfights (space-fighters FD rules).
add_executable(tactical_dogfight_tests dogfight_tests.cpp)
target_link_libraries(tactical_dogfight_tests PRIVATE eawr::sim eawr::platform)
target_compile_features(tactical_dogfight_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(tactical_dogfight_tests)
add_test(
    NAME tactical_dogfight_contracts
    COMMAND tactical_dogfight_tests
)
# #469: the target scans' collection trees (space-targeting CO-01 to CO-12).
add_executable(tactical_collection_tests collection_tests.cpp)
target_link_libraries(tactical_collection_tests PRIVATE eawr::sim eawr::platform)
target_compile_features(tactical_collection_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(tactical_collection_tests)
add_test(
    NAME tactical_collection_contracts
    COMMAND tactical_collection_tests
)
# #80: the viewer's live session (own thread, #276 pool) against the headless goldens.
add_executable(tactical_live_session_tests live_session_tests.cpp)
target_link_libraries(tactical_live_session_tests PRIVATE eawr::sim eawr::platform)
target_compile_features(tactical_live_session_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(tactical_live_session_tests)
add_test(
    NAME tactical_live_session_contracts
    COMMAND tactical_live_session_tests "${EAWR_REPLAY_FIXTURES}"
)
add_test(
    NAME tactical_replay_oracle_fixtures
    COMMAND "${Python3_EXECUTABLE}" "${EAWR_REPLAY_FIXTURES}/generate_tactical_fixture.py" --check
)
add_test(
    NAME replay_contracts
    COMMAND replay_contract_tests "${EAWR_REPLAY_FIXTURES}"
)

add_executable(publish_files_tests publish_files_tests.cpp)
target_link_libraries(publish_files_tests PRIVATE eawr::platform)
target_compile_features(publish_files_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(publish_files_tests)
add_test(NAME sim_headless_output_publication COMMAND publish_files_tests)
add_test(
    NAME sim_headless_replay_contract
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_cli.py"
        --program "$<TARGET_FILE:sim_headless>"
        --fixtures "${EAWR_REPLAY_FIXTURES}"
)
# The synthetic large battle agrees on every tick's state hash for 1, 2, 4, 8 and the
# hardware worker count (docs/simulation.md, phase map).
add_test(
    NAME sim_bench_worker_equality
    COMMAND sim_bench --units 1500 --ticks 6 --warmup 2 --repeat 1
)
add_test(
    NAME replay_hash_comparator_contracts
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_compare_replay_hashes.py"
        --tool "${CMAKE_CURRENT_LIST_DIR}/../../tools/compare_replay_hashes.py"
)

set(EAWR_REPLAY_EVIDENCE_DIR "${CMAKE_BINARY_DIR}/replay-evidence")
add_custom_target(
    replay_evidence
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${EAWR_REPLAY_EVIDENCE_DIR}"
    COMMAND
        "$<TARGET_FILE:sim_headless>"
        --replay "${EAWR_REPLAY_FIXTURES}/original-v1.eawr-replay"
        --hash-out "${EAWR_REPLAY_EVIDENCE_DIR}/workers-1.csv"
        --workers 1
    COMMAND
        "$<TARGET_FILE:sim_headless>"
        --replay "${EAWR_REPLAY_FIXTURES}/original-v1.eawr-replay"
        --hash-out "${EAWR_REPLAY_EVIDENCE_DIR}/workers-2.csv"
        --workers 2
    COMMAND
        "$<TARGET_FILE:sim_headless>"
        --replay "${EAWR_REPLAY_FIXTURES}/original-v1.eawr-replay"
        --hash-out "${EAWR_REPLAY_EVIDENCE_DIR}/workers-4.csv"
        --workers 4
    DEPENDS sim_headless
    VERBATIM
)

# Keep v1 evidence and add the complete tactical presentation/command bundle.
foreach(workers IN ITEMS 1 2 4)
    add_custom_command(TARGET replay_evidence POST_BUILD
        COMMAND "$<TARGET_FILE:sim_headless>"
            --replay "${EAWR_REPLAY_FIXTURES}/tactical-v2.eawr-replay"
            --hash-out "${EAWR_REPLAY_EVIDENCE_DIR}/tactical-v2-workers-${workers}.hashes.csv"
            --events-out "${EAWR_REPLAY_EVIDENCE_DIR}/tactical-v2-workers-${workers}.events.csv"
            --snapshot-out "${EAWR_REPLAY_EVIDENCE_DIR}/tactical-v2-workers-${workers}.snapshots.csv"
            --workers ${workers}
        COMMAND "$<TARGET_FILE:sim_headless>"
            --replay "${PROJECT_SOURCE_DIR}/tests/skirmish/fixtures/m2-start.eawr-replay"
            --hash-out "${EAWR_REPLAY_EVIDENCE_DIR}/skirmish-workers-${workers}.hashes.csv"
            --snapshot-out "${EAWR_REPLAY_EVIDENCE_DIR}/skirmish-workers-${workers}.snapshots.csv"
            --census-out "${EAWR_REPLAY_EVIDENCE_DIR}/skirmish-workers-${workers}.census.json"
            --workers ${workers}
        VERBATIM)
endforeach()
add_executable(tactical_damage_tests damage_tests.cpp)
target_link_libraries(tactical_damage_tests PRIVATE eawr::sim eawr::platform)
target_compile_features(tactical_damage_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(tactical_damage_tests)
add_test(
    NAME tactical_damage_contracts
    COMMAND tactical_damage_tests "${EAWR_REPLAY_FIXTURES}"
)
add_executable(tactical_ion_tests ion_tests.cpp)
target_link_libraries(tactical_ion_tests PRIVATE eawr::sim eawr::platform)
target_compile_features(tactical_ion_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(tactical_ion_tests)
add_test(
    NAME tactical_ion_contracts
    COMMAND tactical_ion_tests
)
add_executable(tactical_ion_shot_tests ion_shot_tests.cpp)
target_link_libraries(tactical_ion_shot_tests PRIVATE eawr::sim eawr::platform)
target_compile_features(tactical_ion_shot_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(tactical_ion_shot_tests)
add_test(
    NAME tactical_ion_shot_contracts
    COMMAND tactical_ion_shot_tests
)
add_executable(tactical_combat_tests combat_tests.cpp)
target_link_libraries(tactical_combat_tests PRIVATE eawr::sim eawr::platform)
target_compile_features(tactical_combat_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(tactical_combat_tests)
add_test(
    NAME tactical_combat_contracts
    COMMAND tactical_combat_tests "${EAWR_REPLAY_FIXTURES}" "${PROJECT_SOURCE_DIR}/tests/behaviour/space-targeting-cases.json"
)

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
