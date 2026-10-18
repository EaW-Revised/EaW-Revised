eawr_add_test_case(
    NAME galactic_scripted_session_tests
    SOURCES scripted_session_tests.cpp
    LIBS eawr::script_galactic eawr::platform
    TESTS galactic_scripted_session_contracts
)

add_test(NAME galactic_lua_api_inventory_contracts
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_lua_api_inventory.py"
)

# sim_headless --galactic and --galactic-replay over the synthetic campaign fixtures.
