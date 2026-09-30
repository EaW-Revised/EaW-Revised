find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

add_test(
    NAME fidelity_trace_comparator_contracts
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_compare_traces.py"
        --tool "${PROJECT_SOURCE_DIR}/tools/compare_traces.py"
        --scenarios "${CMAKE_CURRENT_LIST_DIR}"
        --program "$<TARGET_FILE:sim_headless>"
        --replay "${PROJECT_SOURCE_DIR}/tests/replay/fixtures/original-v1.eawr-replay"
)

# P2-07 (#70): S-10..S-13 and S-17 through sim_headless --scenario with 1, 2 and 4 workers, and
# an attack on an event-spawned target (#392); runs when EAWR_EAW_GAME_ROOT names a FoC installation.
add_test(
    NAME fidelity_movement_scenario_traces
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_scenario_traces.py"
        --program "$<TARGET_FILE:sim_headless>"
        --scenarios "${CMAKE_CURRENT_LIST_DIR}"
        --tool "${PROJECT_SOURCE_DIR}/tools/compare_traces.py"
)

# #561: the S-51 ion drain through sim_headless --scenario with 1, 2, 4 and 8 workers, against the
# recorded outcome; runs when EAWR_EAW_GAME_ROOT names a FoC installation.
add_test(
    NAME fidelity_ion_drain_scenario_trace
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_ion_drain_trace.py"
        --program "$<TARGET_FILE:sim_headless>"
        --scenarios "${CMAKE_CURRENT_LIST_DIR}"
        --tool "${PROJECT_SOURCE_DIR}/tools/compare_traces.py"
)

# #561: the S-52 Y-wing ion shot through sim_headless --scenario with 1, 2, 4 and 8 workers,
# against the recorded outcome; runs when EAWR_EAW_GAME_ROOT names a FoC installation.
add_test(
    NAME fidelity_ion_shot_scenario_trace
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_ion_shot_trace.py"
        --program "$<TARGET_FILE:sim_headless>"
        --scenarios "${CMAKE_CURRENT_LIST_DIR}"
        --tool "${PROJECT_SOURCE_DIR}/tools/compare_traces.py"
)

# P2-11 (#74): the S-15 duel through sim_headless --scenario with 1, 2 and 4 workers, against the
# recorded outcome; runs when EAWR_EAW_GAME_ROOT names a FoC installation.
add_test(
    NAME fidelity_duel_scenario_trace
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_duel_trace.py"
        --program "$<TARGET_FILE:sim_headless>"
        --scenarios "${CMAKE_CURRENT_LIST_DIR}"
        --tool "${PROJECT_SOURCE_DIR}/tools/compare_traces.py"
)

# #361: the rear-arc and attack-order cases S-22 to S-27 through sim_headless --scenario (S-26 and
# S-27 with 1, 2 and 4 workers) against the recorded turn; runs when EAWR_EAW_GAME_ROOT names a
# FoC installation.
add_test(
    NAME fidelity_attack_turn_scenario_traces
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_attack_turn_traces.py"
        --program "$<TARGET_FILE:sim_headless>"
        --scenarios "${CMAKE_CURRENT_LIST_DIR}"
        --tool "${PROJECT_SOURCE_DIR}/tools/compare_traces.py"
)

# P2-12 (#75): the S-28 squadron attack through sim_headless --scenario with 1, 2 and 4 workers,
# against the recorded outcome; runs when EAWR_EAW_GAME_ROOT names a FoC installation.
add_test(
    NAME fidelity_squadron_scenario_trace
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_squadron_trace.py"
        --program "$<TARGET_FILE:sim_headless>"
        --scenarios "${CMAKE_CURRENT_LIST_DIR}"
        --tool "${PROJECT_SOURCE_DIR}/tools/compare_traces.py"
)

# #457, #607: the S-97 squadron dogfight (1, 2, 4 and 8 workers) and the S-98 interception (1 and 8)
# through sim_headless --scenario, against the recorded outcomes; runs when EAWR_EAW_GAME_ROOT names a
# FoC installation.
add_test(
    NAME fidelity_dogfight_scenario_trace
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_dogfight_trace.py"
        --program "$<TARGET_FILE:sim_headless>"
        --scenarios "${CMAKE_CURRENT_LIST_DIR}"
        --tool "${PROJECT_SOURCE_DIR}/tools/compare_traces.py"
)

# #599: the S-57 squadron group move through sim_headless --scenario with 1, 2 and 4 workers:
# in-flight spacing and lane crossings (space-fighters FO-09 to FO-11); runs when
# EAWR_EAW_GAME_ROOT names a FoC installation.
add_test(
    NAME fidelity_group_move_scenario_trace
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_group_move_trace.py"
        --program "$<TARGET_FILE:sim_headless>"
        --scenarios "${CMAKE_CURRENT_LIST_DIR}"
        --tool "${PROJECT_SOURCE_DIR}/tools/compare_traces.py"
)

# #452: the order cases S-29 to S-31 (attack out of range, guard, attack-move) through
# sim_headless --scenario with 1, 2 and 4 workers, against docs/behaviour/space-orders.md; runs
# when EAWR_EAW_GAME_ROOT names a FoC installation.
add_test(
    NAME fidelity_order_scenario_traces
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_order_traces.py"
        --program "$<TARGET_FILE:sim_headless>"
        --scenarios "${CMAKE_CURRENT_LIST_DIR}"
        --tool "${PROJECT_SOURCE_DIR}/tools/compare_traces.py"
)

# #536: time to kill in the one-sided matchups S-43 to S-50 through sim_headless --scenario and
# its combat log, against the FoC recordings; runs when EAWR_EAW_GAME_ROOT names a FoC installation.
add_test(
    NAME fidelity_ttk_scenario_traces
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_ttk_traces.py"
        --program "$<TARGET_FILE:sim_headless>"
        --scenarios "${CMAKE_CURRENT_LIST_DIR}"
)

# Every scenario trace needs the game data: without EAWR_EAW_GAME_ROOT it prints "SKIPPED:" and CTest
# reports it skipped, not passed (open-source plan #445).
get_property(_eawr_fidelity_tests DIRECTORY PROPERTY TESTS)
foreach(_eawr_test IN LISTS _eawr_fidelity_tests)
    if(_eawr_test MATCHES "_scenario_traces?$")
        set_tests_properties(${_eawr_test} PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
    endif()
endforeach()

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
