eawr_add_test_case(
    NAME galactic_space_entry_tests
    SOURCES space_entry_tests.cpp
    LIBS eawr::campaign eawr::platform eawr::checkpoint
    TESTS galactic_space_entry
)

add_test(NAME galactic_space_entry_stock COMMAND galactic_space_entry_tests --installed)
add_test(NAME galactic_space_commander_stock COMMAND galactic_space_entry_tests --installed-commanders)
set_tests_properties(galactic_space_commander_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME galactic_space_population_stock COMMAND galactic_space_entry_tests --installed-space-population)
set_tests_properties(galactic_space_population_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
set_tests_properties(galactic_space_entry_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME galactic_space_garrison_stock COMMAND galactic_space_entry_tests --installed-garrison)
add_test(NAME galactic_space_superweapon_stock COMMAND galactic_space_entry_tests --installed-superweapon)
set_tests_properties(galactic_space_superweapon_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
set_tests_properties(galactic_space_garrison_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
