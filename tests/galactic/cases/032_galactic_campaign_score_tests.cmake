eawr_add_test_case(
    NAME galactic_campaign_score_tests
    SOURCES campaign_score_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    LIBS eawr::campaign eawr::platform eawr::data eawr::vfs
    TESTS galactic_campaign_score_contracts
)
