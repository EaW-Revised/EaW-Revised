eawr_add_test_case(
    NAME galactic_game_session_tests
    SOURCES game_session_tests.cpp
    LIBS eawr::campaign eawr::platform
    TESTS galactic_game_session_contracts
)
