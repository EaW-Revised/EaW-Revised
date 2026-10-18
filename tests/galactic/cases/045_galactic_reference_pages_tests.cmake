eawr_add_test_case(
    NAME galactic_reference_pages_tests
    SOURCES reference_pages_tests.cpp
        "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/reference_catalog.cpp"
        "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/reference_pages.cpp"
        "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/reference_publication.cpp"
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    LIBS eawr::sim_galactic eawr::data eawr::vfs
    TESTS galactic_reference_pages_contracts
)

eawr_add_python_contract(python_presentation_renderer_galactic_reference_pages_runtime
    tests.presentation.renderer.test_viewer_galactic_reference_pages_runtime)
set_tests_properties(python_presentation_renderer_galactic_reference_pages_runtime PROPERTIES LABELS gpu)
