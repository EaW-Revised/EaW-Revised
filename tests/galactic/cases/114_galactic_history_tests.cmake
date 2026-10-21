eawr_add_test_case(
    NAME galactic_history_tests
    SOURCES history_tests.cpp
    LIBS eawr::script_galactic eawr::platform eawr::campaign
    TESTS galactic_history_contracts
)

eawr_add_python_contract(python_presentation_renderer_galactic_history_runtime
    tests.presentation.renderer.test_viewer_galactic_history_runtime)
set_tests_properties(python_presentation_renderer_galactic_history_runtime PROPERTIES LABELS gpu)
