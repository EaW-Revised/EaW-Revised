eawr_add_galactic_area_case(
    AREA galactic_area_ai_observation
    NO_ARGS

    NAME galactic_ai_lua_fault_tests
    SOURCES ai_lua_fault_stock_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::checkpoint eawr::platform
)

# One Lua error in a free-store callback or a plan thread ends that call or thread only: the stock campaign with injected
# faults keeps every AI instance and player, and plans keep starting.
eawr_add_game_data_test(NAME galactic_ai_lua_fault_stock COMMAND galactic_area_ai_observation galactic_ai_lua_fault_tests)
set_tests_properties(galactic_ai_lua_fault_stock PROPERTIES LABELS "game-data;observation" TIMEOUT 1800
    ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
