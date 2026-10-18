eawr_add_test_case(
    NAME gc_lockstep_loopback_tests
    SOURCES lockstep_loopback_tests.cpp
    LIBS eawr::sim eawr::platform
)

add_test(
    NAME gc_lockstep_loopback
    COMMAND gc_lockstep_loopback_tests
        "${PROJECT_SOURCE_DIR}/tests/fidelity/fixtures"
        "${PROJECT_SOURCE_DIR}/tests/replay/fixtures"
        "${PROJECT_SOURCE_DIR}/tests/skirmish/fixtures"
)

# Engine-free view contracts over immutable live snapshots.
