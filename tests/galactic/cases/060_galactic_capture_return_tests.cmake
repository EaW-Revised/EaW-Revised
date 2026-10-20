eawr_add_galactic_area_case(
    AREA galactic_area_campaign_transitions
    CONST_ARGV

    NAME galactic_capture_return_tests
    SOURCES capture_return_tests.cpp
    LIBS eawr::campaign eawr::platform eawr::checkpoint
    TESTS galactic_capture_return_stock
)

# The 18 installed invasions include 1,500-frame holds and 1/2/4/8 worker equality.
# This matrix already measured 1,341 seconds before the build-list checks; allow margin on busy hosts.
set_tests_properties(galactic_capture_return_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:" TIMEOUT 3600)
eawr_add_game_data_test(NAME galactic_capture_build_list_empire_stock COMMAND galactic_area_campaign_transitions galactic_capture_return_tests --build-list Empire)
add_test(NAME galactic_capture_build_list_rebel_stock COMMAND galactic_area_campaign_transitions galactic_capture_return_tests --build-list Rebel)
add_test(NAME galactic_capture_build_list_renamed_stock COMMAND galactic_area_campaign_transitions galactic_capture_return_tests --build-list RenamedBuilder)
add_test(NAME galactic_capture_build_list_synthetic COMMAND galactic_control_tests --capture-build-list)
set_tests_properties(galactic_capture_build_list_empire_stock galactic_capture_build_list_rebel_stock galactic_capture_build_list_renamed_stock
    PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:" LABELS game-data)
eawr_add_python_contract(python_presentation_renderer_capture_runtime tests.presentation.renderer.test_viewer_capture_runtime)
set_tests_properties(python_presentation_renderer_capture_runtime PROPERTIES LABELS gpu)
