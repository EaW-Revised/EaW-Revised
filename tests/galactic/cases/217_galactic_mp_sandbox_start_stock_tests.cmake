eawr_add_test_case(
    NAME galactic_mp_sandbox_start_stock_tests
    SOURCES mp_sandbox_start_stock_tests.cpp
    LIBS eawr::campaign eawr::platform
)

# #7058: each stock multiplayer sandbox starts live with one local human, as the lobby seats it.
foreach(entry Origin Gateways Equal_Footing Heart_Of_The_Maw Rim_Worlds Clusters)
    eawr_add_game_data_test(NAME galactic_mp_sandbox_start_stock_${entry}
        COMMAND galactic_mp_sandbox_start_stock_tests Multiplayer_Sandbox_${entry})
    set_tests_properties(galactic_mp_sandbox_start_stock_${entry} PROPERTIES LABELS game-data TIMEOUT 900
        SKIP_REGULAR_EXPRESSION "SKIPPED:")
endforeach()
# Every playable faction a human side, only ambient AI seats left: the start runs without a native AI (GCMP-AI-03).
eawr_add_game_data_test(NAME galactic_mp_sandbox_start_stock_ambient_only
    COMMAND galactic_mp_sandbox_start_stock_tests Multiplayer_Sandbox_Origin --ambient-only)
set_tests_properties(galactic_mp_sandbox_start_stock_ambient_only PROPERTIES LABELS game-data TIMEOUT 900
    SKIP_REGULAR_EXPRESSION "SKIPPED:")
