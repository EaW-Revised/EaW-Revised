eawr_add_galactic_area_case(
    AREA galactic_area_checkpoint

    NAME galactic_live_save_tests
    SOURCES live_save_tests.cpp
    LIBS eawr::checkpoint eawr::platform
)

add_test(NAME galactic_live_save_roundtrip COMMAND galactic_area_checkpoint galactic_live_save_tests "${CMAKE_CURRENT_BINARY_DIR}/live-save-contract")
eawr_add_python_contract(python_presentation_renderer_galactic_save_runtime tests.presentation.renderer.test_viewer_galactic_save_runtime)
set_tests_properties(python_presentation_renderer_galactic_save_runtime PROPERTIES LABELS gpu)
