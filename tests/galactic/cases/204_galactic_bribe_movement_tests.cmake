eawr_add_test_case(
    NAME galactic_bribe_movement_tests
    SOURCES bribe_movement_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
)
add_test(NAME galactic_bribe_movement_contracts COMMAND galactic_bribe_movement_tests)
