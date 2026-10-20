eawr_add_galactic_area_case(
    AREA galactic_area_live_campaign

    NAME galactic_s2_loop_tests
    SOURCES s2_loop_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    LIBS eawr::campaign eawr::platform eawr::presentation
    TESTS galactic_s2_loop
)

set_tests_properties(galactic_s2_loop PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME galactic_purchase_ground_roster COMMAND galactic_area_live_campaign galactic_s2_loop_tests --purchase-only)
set_tests_properties(galactic_purchase_ground_roster PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
