eawr_add_test_case(
    NAME galactic_auto_resolve_round_tests
    SOURCES auto_resolve_round_tests.cpp
    LIBS eawr::campaign eawr::platform eawr::script_foc
    TESTS galactic_auto_resolve_round
)

add_test(NAME galactic_auto_resolve_round_stock COMMAND galactic_auto_resolve_round_tests --installed)
set_tests_properties(galactic_auto_resolve_round_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
