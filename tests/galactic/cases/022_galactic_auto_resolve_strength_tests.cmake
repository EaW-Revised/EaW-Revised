eawr_add_test_case(
    NAME galactic_auto_resolve_strength_tests
    SOURCES auto_resolve_strength_tests.cpp
    LIBS eawr::campaign eawr::platform
    TESTS galactic_auto_resolve_strength
)

add_test(NAME galactic_auto_resolve_strength_stock COMMAND galactic_auto_resolve_strength_tests --installed)
set_tests_properties(galactic_auto_resolve_strength_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")

# FT-06: the two-peer lockstep loopback gate, over the committed tactical replay corpus first.
