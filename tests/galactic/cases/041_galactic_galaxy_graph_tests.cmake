eawr_add_test_case(
    NAME galactic_galaxy_graph_tests
    SOURCES galaxy_graph_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_galaxy_graph_contracts
)

add_test(NAME galactic_galaxy_graph_stock COMMAND galactic_galaxy_graph_tests --installed)
set_tests_properties(galactic_galaxy_graph_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME galactic_graph_replay_corpus
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/replay_corpus_check.py"
        --program "$<TARGET_FILE:sim_headless>"
        --corpus "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/graph-corpus")
