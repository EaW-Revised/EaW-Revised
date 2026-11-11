# GFS-A22 / S9b-2: ordered AI-intent commit (accepted and refused intents of every kind, 1/2/4/8 workers).
eawr_add_galactic_area_case(
    AREA galactic_area_ai_components
    NO_ARGS

    NAME galactic_ai_intent_commit_tests
    SOURCES ai_intent_commit_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::script_foc eawr::platform
    TESTS galactic_ai_intent_commit_contracts
)
