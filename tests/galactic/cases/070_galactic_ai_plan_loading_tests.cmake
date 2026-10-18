eawr_add_test_case(
    NAME galactic_ai_plan_loading_tests
    SOURCES ai_plan_loading_stock_tests.cpp ai_goal_state_stock_tests.cpp ai_learning_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::script_foc eawr::vfs eawr::campaign eawr::platform
    TESTS galactic_ai_plan_loading_contracts
)

eawr_add_game_data_test(NAME galactic_ai_plan_loading_stock COMMAND galactic_ai_plan_loading_tests --installed)
set_tests_properties(galactic_ai_plan_loading_stock PROPERTIES LABELS game-data)
# Supporting live ledger hooks; installed stock-plan acceptance is composed by S4.
