eawr_add_python_contract(python_presentation_renderer_galactic_build_tray_runtime
    tests.presentation.renderer.test_viewer_galactic_build_tray_runtime)
set_tests_properties(python_presentation_renderer_galactic_build_tray_runtime PROPERTIES LABELS gpu)
# GBT-07: the tray key's refusal set covers every key the galaxy view handles (source scan, no engine).
add_test(NAME galactic_build_tray_key_pin
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/galactic/test_build_tray_key_pin.py")
