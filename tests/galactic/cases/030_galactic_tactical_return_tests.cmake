eawr_add_test_case(
    NAME galactic_tactical_return_tests
    SOURCES tactical_return_tests.cpp
    LIBS eawr::campaign eawr::platform
    TESTS galactic_tactical_return
)

eawr_add_game_data_test(NAME galactic_tactical_return_stock COMMAND galactic_tactical_return_tests --installed)
set_tests_properties(galactic_tactical_return_stock PROPERTIES LABELS game-data)
