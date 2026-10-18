eawr_add_test_case(
    NAME galactic_ai_plans_tests
    SOURCES ai_plans_stock_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::script_foc eawr::platform
    TESTS galactic_ai_plans_contracts
)

# Definition/data proof; strict installed mode requires the verified game mirror.
