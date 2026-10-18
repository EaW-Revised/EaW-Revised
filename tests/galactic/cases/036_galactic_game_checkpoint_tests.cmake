eawr_add_test_case(
    NAME galactic_game_checkpoint_tests
    SOURCES game_checkpoint_tests.cpp
    LIBS eawr::checkpoint eawr::platform
)

add_test(NAME galactic_timeline_contracts COMMAND galactic_game_checkpoint_tests --timeline)
add_test(NAME galactic_pending_conflict_barrier COMMAND galactic_game_checkpoint_tests --pending-conflict)
add_test(NAME galactic_game_checkpoint_contracts COMMAND galactic_game_checkpoint_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/corpus")

add_test(
    NAME galactic_cli_contract
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/galactic_cli_check.py"
        --program "$<TARGET_FILE:sim_headless>"
        --fixtures "${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
