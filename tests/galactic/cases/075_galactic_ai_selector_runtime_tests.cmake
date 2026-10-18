eawr_add_test_case(
    NAME galactic_ai_selector_runtime_tests
    SOURCES ai_selector_runtime_tests.cpp ai_synchronized_staging_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::script_foc eawr::platform
    TESTS galactic_ai_selector_runtime_contracts
)
