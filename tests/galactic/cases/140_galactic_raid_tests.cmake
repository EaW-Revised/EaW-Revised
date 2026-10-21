eawr_add_test_case(
    NAME galactic_raid_tests
    SOURCES raid_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
)
add_test(NAME galactic_raid_contracts COMMAND galactic_raid_tests)
