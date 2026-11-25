eawr_add_test_case(
    NAME galactic_fleet_popup_title_tests
    SOURCES fleet_popup_title_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/apps/viewer/src" "${PROJECT_SOURCE_DIR}/tests/ui"
    LIBS eawr::ui_data eawr::ui_model
    TESTS galactic_fleet_popup_title
)
