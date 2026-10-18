eawr_add_test_case(
    NAME galactic_viewer_view_tests
    SOURCES galactic_viewer_view_tests.cpp
        "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/galaxy_view.cpp"
        "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/galaxy_snapshot_stub.cpp"
        "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/planet_hud.cpp"
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    LIBS eawr::sim_galactic eawr::platform
    TESTS galactic_viewer_view_contracts
)
