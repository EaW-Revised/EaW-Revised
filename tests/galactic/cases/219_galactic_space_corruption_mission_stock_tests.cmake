eawr_add_test_case(
    NAME galactic_space_corruption_mission_stock_tests
    SOURCES space_corruption_mission_stock_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    LIBS eawr::campaign eawr::platform eawr::presentation
)

# #7357 / WGCH-126/197: a solo human's stock space corruption choice (Piracy) plays its mission battle on its own map,
# through the player's route (drop, Corrupt Planet menu, execute) on 1/2/4/8 workers.
eawr_add_game_data_test(NAME galactic_space_corruption_mission_stock COMMAND galactic_space_corruption_mission_stock_tests)
set_tests_properties(galactic_space_corruption_mission_stock PROPERTIES LABELS game-data SKIP_REGULAR_EXPRESSION "SKIPPED:" TIMEOUT 1800)
