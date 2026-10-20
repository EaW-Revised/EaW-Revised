eawr_add_galactic_area_case(
    AREA galactic_area_ai_execution
    NO_ARGS

    NAME galactic_ai_world_reader_tests
    SOURCES ai_world_reader_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::script_foc eawr::campaign eawr::platform
    TESTS galactic_ai_world_reader_contracts
)

# WGA-485: bounded stock observation, matching the seed-1 idle-human day gate.
