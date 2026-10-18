eawr_add_test_case(
    NAME galactic_route_tests
    SOURCES galactic_route_tests.cpp
        "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/route_lines.cpp"
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    LIBS eawr::presentation
    TESTS galactic_route_contracts
)

eawr_add_python_contract(python_presentation_renderer_galaxy_routes_runtime tests.presentation.renderer.test_viewer_galaxy_routes_runtime)
set_tests_properties(python_presentation_renderer_galaxy_routes_runtime PROPERTIES LABELS gpu)

# The Python suite runs its source checks in root CTest and its graphical
# cases only when opted in on a GPU host.
set_tests_properties(galactic_viewer_mode_contracts PROPERTIES LABELS gpu)
