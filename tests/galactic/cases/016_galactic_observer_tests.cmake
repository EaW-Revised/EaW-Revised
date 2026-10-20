eawr_add_galactic_area_case(
    AREA galactic_area_campaign_economy
    NO_ARGS

    NAME galactic_observer_tests
    SOURCES observer_tests.cpp support_self_test.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_observer
)
