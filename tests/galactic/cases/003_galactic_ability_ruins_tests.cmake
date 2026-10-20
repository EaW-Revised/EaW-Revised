eawr_add_galactic_area_case(
    AREA galactic_area_abilities
    NO_ARGS

    NAME galactic_ability_ruins_tests
    SOURCES ability_ruins_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_ability_ruins_contracts
)
