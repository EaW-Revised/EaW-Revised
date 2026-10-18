eawr_add_test_case(
    NAME galactic_ai_frontier_runtime_tests
    SOURCES ai_frontier_runtime_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::checkpoint eawr::platform
)

eawr_add_game_data_test(NAME galactic_ai_frontier_stock COMMAND galactic_ai_frontier_runtime_tests)
set_tests_properties(galactic_ai_frontier_stock PROPERTIES LABELS game-data ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0" TIMEOUT 3600)

# #3903: single-record perception encoders equal the whole-state encoder and allocate nothing warm.
