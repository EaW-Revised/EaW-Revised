eawr_add_test_case(
    NAME galactic_economy_corruption_tests
    SOURCES economy_corruption_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform eawr::vfs
    TESTS galactic_economy_corruption_contracts
)

eawr_add_game_data_test(NAME galactic_economy_corruption_stock COMMAND galactic_economy_corruption_tests --installed)
set_tests_properties(galactic_economy_corruption_contracts PROPERTIES ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
set_tests_properties(galactic_economy_corruption_stock PROPERTIES LABELS game-data ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
eawr_add_python_contract(python_presentation_renderer_galactic_hero_actions_runtime tests.presentation.renderer.test_viewer_galactic_hero_actions_runtime)
