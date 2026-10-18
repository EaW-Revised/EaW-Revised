eawr_add_test_case(
    NAME galactic_seat_roles_tests
    SOURCES seat_roles_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_seat_roles_contracts
)
