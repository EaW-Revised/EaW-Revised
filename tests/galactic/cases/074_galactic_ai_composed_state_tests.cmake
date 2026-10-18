eawr_add_test_case(
    NAME galactic_ai_composed_state_tests
    SOURCES ai_composed_state_tests.cpp
    INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/src/script/foc"
    LIBS eawr::checkpoint eawr::platform
    TESTS galactic_ai_composed_state_contracts
)

eawr_add_game_data_test(NAME galactic_ai_composed_state_stock COMMAND galactic_ai_composed_state_tests --installed)
set_tests_properties(galactic_ai_composed_state_contracts PROPERTIES ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
set_tests_properties(galactic_ai_composed_state_stock PROPERTIES LABELS game-data ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
# The same frozen stock state measures the full-capture baseline and the Release
# incremental path. Correctness cases above keep verification on every request.
eawr_add_game_data_test(NAME galactic_ai_digest_benchmark_stock COMMAND galactic_ai_composed_state_tests --benchmark)
set_tests_properties(galactic_ai_digest_benchmark_stock PROPERTIES LABELS game-data ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")

# WGA-01..05/GFD-A04: lane A shared copied-input and capability contract.
