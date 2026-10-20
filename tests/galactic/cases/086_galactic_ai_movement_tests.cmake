eawr_add_galactic_area_case(
    AREA galactic_area_ai_movement

    NAME galactic_ai_movement_tests
    SOURCES ai_movement_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::script_foc eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_ai_movement_contracts
)

eawr_add_game_data_test(NAME galactic_ai_movement_stock COMMAND galactic_area_ai_movement galactic_ai_movement_tests --installed)
set_tests_properties(galactic_ai_movement_stock PROPERTIES LABELS game-data)

# Lane B component proof; lifecycle/actions and retained-route writer compose later.
