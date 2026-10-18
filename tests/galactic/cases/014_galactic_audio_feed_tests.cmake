eawr_add_test_case(
    NAME galactic_audio_feed_tests
    SOURCES audio_feed_tests.cpp
    LIBS eawr::sim_galactic eawr::platform eawr::presentation
    TESTS galactic_audio_feed_contracts
)

eawr_add_python_contract(python_presentation_renderer_galactic_audio tests.presentation.renderer.test_galactic_audio)
set_tests_properties(python_presentation_renderer_galactic_audio PROPERTIES LABELS gpu)
