eawr_add_galactic_area_case(
    AREA galactic_area_ai_components
    NO_ARGS

    NAME galactic_ai_stream_ids_tests
    SOURCES ai_stream_ids_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::script_foc eawr::platform
    TESTS galactic_ai_stream_ids_contracts
)
