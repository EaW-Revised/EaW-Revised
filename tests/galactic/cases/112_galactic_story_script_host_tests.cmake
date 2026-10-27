eawr_add_galactic_area_case(
    AREA galactic_area_campaign_transitions

    NAME galactic_story_script_host_tests
    SOURCES story_script_host_tests.cpp
    LIBS eawr::campaign eawr::platform eawr::checkpoint
)

add_test(NAME galactic_story_script_host_contracts COMMAND galactic_area_campaign_transitions galactic_story_script_host_tests "${CMAKE_CURRENT_BINARY_DIR}/story-script-host")
