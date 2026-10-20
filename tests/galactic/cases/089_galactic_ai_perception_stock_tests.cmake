eawr_add_galactic_area_case(
    AREA galactic_area_ai_execution

    NAME galactic_ai_perception_stock_tests
    SOURCES ai_perception_stock_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::script_foc eawr::campaign eawr::platform
    TESTS galactic_ai_perception_contracts
)

add_test(NAME galactic_ai_perception_stock COMMAND galactic_area_ai_execution galactic_ai_perception_stock_tests --installed)
set_tests_properties(galactic_ai_perception_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
