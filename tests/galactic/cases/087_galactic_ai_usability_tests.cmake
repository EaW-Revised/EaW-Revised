eawr_add_test_case(
    NAME galactic_ai_usability_tests
    SOURCES ai_usability_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc" "${PROJECT_SOURCE_DIR}/src/campaign"
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_ai_usability_contracts
)

set_tests_properties(galactic_ai_usability_contracts PROPERTIES ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
