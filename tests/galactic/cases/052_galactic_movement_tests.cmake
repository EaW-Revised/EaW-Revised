eawr_add_galactic_area_case(
    AREA galactic_area_campaign_transitions
    CONST_ARGV

    NAME galactic_movement_tests
    SOURCES movement_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_movement_contracts
)

add_test(NAME galactic_movement_stock COMMAND galactic_area_campaign_transitions galactic_movement_tests --installed)
set_tests_properties(galactic_movement_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME galactic_movement_replay_corpus
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/replay_corpus_check.py"
        --program "$<TARGET_FILE:sim_headless>"
        --corpus "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/movement-corpus")
