eawr_add_galactic_area_case(
    AREA galactic_area_story_filters

    NAME galactic_story_filter_flow_tests
    SOURCES story_filter_flow_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    LIBS eawr::campaign eawr::platform eawr::checkpoint eawr::sim_galactic eawr::ui_data
)

add_test(NAME galactic_story_filter_flow_contracts COMMAND galactic_area_story_filters galactic_story_filter_flow_tests "${CMAKE_CURRENT_BINARY_DIR}/story-filter-flow")
