eawr_add_test_case(
    NAME galactic_shared_control_tests
    SOURCES shared_control_tests.cpp
    LIBS eawr::skirmish eawr::platform
    TESTS galactic_shared_control
)

add_test(NAME galactic_shared_control_m2 COMMAND galactic_shared_control_tests --installed)
set_tests_properties(galactic_shared_control_m2 PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")

# Author synthetic fixtures with the same public GameSession API as the CLI runner.
