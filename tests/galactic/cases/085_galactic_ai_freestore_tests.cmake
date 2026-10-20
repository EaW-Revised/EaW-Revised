eawr_add_galactic_area_case(
    AREA galactic_area_ai_movement

    NAME galactic_ai_freestore_tests
    SOURCES ai_freestore_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::script_foc eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_ai_freestore_contracts
)

eawr_add_game_data_test(NAME galactic_ai_freestore_stock COMMAND galactic_area_ai_movement galactic_ai_freestore_tests --installed)
set_tests_properties(galactic_ai_freestore_contracts PROPERTIES ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
set_tests_properties(galactic_ai_freestore_stock PROPERTIES LABELS game-data ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
