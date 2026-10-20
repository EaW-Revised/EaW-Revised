eawr_add_galactic_area_case(
    AREA galactic_area_game_session
    NO_ARGS

    NAME galactic_live_game_session_tests
    SOURCES live_game_session_tests.cpp
    LIBS eawr::campaign eawr::platform
    TESTS galactic_live_game_session_contracts
)
