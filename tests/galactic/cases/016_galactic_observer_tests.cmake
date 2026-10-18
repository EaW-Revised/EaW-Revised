eawr_add_test_case(
    NAME galactic_observer_tests
    SOURCES observer_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_observer
)
