eawr_add_test_case(
    NAME galactic_departure_piracy_tests
    SOURCES departure_piracy_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
)
add_test(NAME galactic_departure_piracy_contracts COMMAND galactic_departure_piracy_tests)
