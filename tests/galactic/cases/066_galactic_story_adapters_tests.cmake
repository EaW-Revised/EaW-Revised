eawr_add_test_case(
    NAME galactic_story_adapters_tests
    SOURCES story_adapters_tests.cpp
    LIBS eawr::sim_galactic eawr::data eawr::vfs
    TESTS galactic_story_adapters_contracts
)

add_test(NAME galactic_story_adapters_stock COMMAND galactic_story_adapters_tests --installed)
set_tests_properties(galactic_story_adapters_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
# WGA-11..24/53..54 fixture-only category/goal funding ledger.
