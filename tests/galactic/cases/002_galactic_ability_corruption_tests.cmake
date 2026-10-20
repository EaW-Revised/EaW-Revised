eawr_add_galactic_area_case(
    AREA galactic_area_abilities
    NO_ARGS

    NAME galactic_ability_corruption_tests
    SOURCES ability_corruption_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_ability_corruption_contracts
)
