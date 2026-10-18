eawr_add_test_case(
    NAME galactic_ai_economic_replay_tests
    SOURCES ai_economic_replay_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::script_foc eawr::platform
    TESTS galactic_ai_economic_replay_contracts
)

# WGA-56/139: F native checkpoint/live disk boundary contracts, no stock proof.
