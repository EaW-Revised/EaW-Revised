eawr_add_test_case(
    NAME galactic_ai_policy_snapshot_tests
    SOURCES ai_policy_snapshot_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::script_foc eawr::script_galactic eawr::platform
    TESTS galactic_ai_policy_snapshot_contracts
)

# WGA-27..32/61/77/78: execution route publication and stock freestore admission.
