eawr_add_test_case(
    NAME galactic_fleet_hud_fixture_tests
    SOURCES fleet_hud_fixture_tests.cpp
        "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/galaxy_view.cpp"
        "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/galaxy_snapshot_stub.cpp"
    LIBS eawr::sim_galactic eawr::platform
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    TESTS galactic_fleet_hud_fixture
)
