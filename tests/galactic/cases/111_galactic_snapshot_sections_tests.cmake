eawr_add_galactic_area_case(
    AREA galactic_area_session
    NO_ARGS

    NAME galactic_snapshot_sections_tests
    SOURCES snapshot_sections_tests.cpp
    LIBS eawr::sim_galactic eawr::platform
    TESTS galactic_snapshot_sections
)
