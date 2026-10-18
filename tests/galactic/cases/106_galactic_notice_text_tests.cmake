eawr_add_test_case(
    NAME galactic_notice_text_tests
    SOURCES notice_text_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/apps/viewer/src" "${PROJECT_SOURCE_DIR}/tests/ui"
    LIBS eawr::ui_data
    TESTS galactic_notice_text
)

set_tests_properties(galactic_notice_text PROPERTIES LABELS "game-data" SKIP_REGULAR_EXPRESSION "corpus: skipped")
