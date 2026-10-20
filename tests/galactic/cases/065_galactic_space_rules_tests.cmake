eawr_add_galactic_area_case(
    AREA galactic_area_campaign_transitions

    NAME galactic_space_rules_tests
    SOURCES space_rules_tests.cpp
    LIBS eawr::campaign eawr::platform eawr::checkpoint
    TESTS galactic_space_rules
)

add_test(NAME galactic_space_rules_stock COMMAND galactic_area_campaign_transitions galactic_space_rules_tests --installed)
set_tests_properties(galactic_space_rules_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")


# WST-30..45/52 fixture story boundary, independent of live session composition.
