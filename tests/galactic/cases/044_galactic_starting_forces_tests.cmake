eawr_add_test_case(
    NAME galactic_starting_forces_tests
    SOURCES starting_forces_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_starting_forces_contracts
)

add_test(NAME galactic_campaign_setup_stock COMMAND galactic_campaign_setup_tests --installed)
set_tests_properties(galactic_campaign_setup_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")

add_test(NAME galactic_fleet_replay_corpus
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/replay_corpus_check.py"
        --program "$<TARGET_FILE:sim_headless>"
        --corpus "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/fleet-corpus")
