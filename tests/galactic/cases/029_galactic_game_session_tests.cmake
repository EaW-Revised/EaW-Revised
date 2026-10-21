eawr_add_galactic_area_case(
    AREA galactic_area_game_session
    NO_ARGS

    NAME galactic_game_session_tests
    SOURCES game_session_tests.cpp
    LIBS eawr::campaign eawr::platform
    TESTS galactic_game_session_contracts
)

eawr_add_galactic_area_case(
    AREA galactic_area_conflict_dispatch
    NO_ARGS

    NAME galactic_conflict_dispatch_tests
    SOURCES conflict_dispatch_tests.cpp
    LIBS eawr::campaign eawr::platform eawr::checkpoint
    TESTS galactic_conflict_dispatch_contracts
)
