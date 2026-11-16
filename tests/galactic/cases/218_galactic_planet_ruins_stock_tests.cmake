eawr_add_test_case(
    NAME galactic_planet_ruins_stock_tests
    SOURCES planet_ruins_stock_tests.cpp
    LIBS eawr::campaign eawr::platform
)

# #7187: a destroyed stock planet is a simulation fact, kept across a save and a load.
eawr_add_game_data_test(NAME galactic_planet_ruins_stock COMMAND galactic_planet_ruins_stock_tests)
set_tests_properties(galactic_planet_ruins_stock PROPERTIES LABELS game-data SKIP_REGULAR_EXPRESSION "SKIPPED:")
