eawr_add_galactic_area_case(
    AREA galactic_area_campaign_transitions
    NO_ARGS

    NAME galactic_raid_live_tests
    SOURCES raid_live_tests.cpp
    LIBS eawr::campaign eawr::platform eawr::checkpoint
    TESTS galactic_raid_live_stock
)
# #6152: one stock solo raid, launched, moved, landed and auto-resolved on 1 and 4 workers.
set_tests_properties(galactic_raid_live_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:" TIMEOUT 1800 LABELS game-data)
