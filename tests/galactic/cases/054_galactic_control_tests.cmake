eawr_add_test_case(
    NAME galactic_control_tests
    SOURCES control_tests.cpp
        "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/planet_hud.cpp"
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_control_contracts
)

add_test(NAME galactic_control_stock COMMAND galactic_control_tests --installed)
set_tests_properties(galactic_control_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME galactic_control_replay_corpus
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/replay_corpus_check.py"
        --program "$<TARGET_FILE:sim_headless>"
        --corpus "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/control-corpus")

# #2880: the seat's surface launch (galactic opcode 18) over the shared native launch owner.
