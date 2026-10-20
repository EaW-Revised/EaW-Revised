eawr_add_galactic_area_case(
    AREA galactic_area_actions
    NO_ARGS

    NAME galactic_ai_funding_tests
    SOURCES ai_funding_tests.cpp
    LIBS eawr::sim_galactic
    TESTS galactic_ai_funding_contracts
)

# WST-47/58 fixture goals and presentation sink.
