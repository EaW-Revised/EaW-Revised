eawr_add_test_case(
    NAME galactic_surface_launch_tests
    SOURCES surface_launch_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_surface_launch_contracts
)
