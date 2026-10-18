eawr_add_test_case(
    NAME galactic_auto_resolve_live_tests
    SOURCES auto_resolve_live_tests.cpp
    LIBS eawr::campaign eawr::platform eawr::checkpoint
    TESTS galactic_auto_resolve_live
)

add_test(NAME galactic_auto_resolve_live_stock COMMAND galactic_auto_resolve_live_tests --installed)
set_tests_properties(galactic_auto_resolve_live_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
