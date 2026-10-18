eawr_add_test_case(
    NAME galactic_live_game_session_tests
    SOURCES live_game_session_tests.cpp
    LIBS eawr::campaign eawr::platform
    TESTS galactic_live_game_session_contracts
)
