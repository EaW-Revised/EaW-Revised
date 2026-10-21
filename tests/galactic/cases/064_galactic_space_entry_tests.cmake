eawr_add_galactic_area_case(
    AREA galactic_area_campaign_transitions

    NAME galactic_space_entry_tests
    SOURCES space_entry_tests.cpp
        space_entry_synthetic.cpp
        space_entry_stock.cpp
    LIBS eawr::campaign eawr::platform eawr::checkpoint
    TESTS galactic_space_entry
)

add_test(NAME galactic_space_entry_stock COMMAND galactic_area_campaign_transitions galactic_space_entry_tests --installed)
add_test(NAME galactic_conflict_dispatch_stock COMMAND galactic_area_campaign_transitions galactic_space_entry_tests --installed-layers)
set_tests_properties(galactic_conflict_dispatch_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME galactic_mp_space_difficulty_stock COMMAND galactic_area_campaign_transitions galactic_space_entry_tests --installed-mp-difficulty)
set_tests_properties(galactic_mp_space_difficulty_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:" TIMEOUT 180)
add_test(NAME galactic_space_commander_stock COMMAND galactic_area_campaign_transitions galactic_space_entry_tests --installed-commanders)
set_tests_properties(galactic_space_commander_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME galactic_space_population_stock COMMAND galactic_area_campaign_transitions galactic_space_entry_tests --installed-space-population)
set_tests_properties(galactic_space_population_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
set_tests_properties(galactic_space_entry_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME galactic_space_garrison_stock COMMAND galactic_area_campaign_transitions galactic_space_entry_tests --installed-garrison)
add_test(NAME galactic_space_orphan_rider_stock COMMAND galactic_area_campaign_transitions galactic_space_entry_tests --installed-orphan-rider)
set_tests_properties(galactic_space_orphan_rider_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME galactic_space_superweapon_stock COMMAND galactic_area_campaign_transitions galactic_space_entry_tests --installed-superweapon)
set_tests_properties(galactic_space_superweapon_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
set_tests_properties(galactic_space_garrison_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
