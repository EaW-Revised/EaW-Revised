eawr_add_galactic_area_case(
    AREA galactic_area_game_session
    NO_ARGS

    NAME galactic_invasion_publication_tests
    SOURCES invasion_publication_tests.cpp
    LIBS eawr::campaign eawr::platform
    TESTS galactic_invasion_publication
)

eawr_add_python_contract(python_presentation_renderer_live_launch_slot_roster tests.presentation.renderer.test_live_launch_slot_roster)
eawr_add_python_contract(python_presentation_renderer_gc_space_cycle tests.presentation.renderer.test_viewer_gc_space_cycle)
set_tests_properties(python_presentation_renderer_gc_space_cycle PROPERTIES LABELS gpu)
