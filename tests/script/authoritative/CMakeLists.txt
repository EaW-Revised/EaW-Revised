# Authoritative Lua sandbox and scheduler (#247, docs/lua-sandbox.md).
add_executable(lua_sandbox_tests lua_sandbox_tests.cpp
    lua_scheduler_tests.cpp)
target_link_libraries(
    lua_sandbox_tests
    PRIVATE eawr::script_authoritative eawr::script_numeric eawr::lua_502_sf eawr::platform eawr::sim eawr::core
)
target_compile_features(lua_sandbox_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(lua_sandbox_tests)

foreach(_eawr_mode IN ITEMS clock_rng sandbox iteration scheduler workers)
    add_test(NAME lua_sandbox_${_eawr_mode} COMMAND lua_sandbox_tests ${_eawr_mode})
endforeach()

# Per-tick script service cost against the #246 budget (docs/lua-sandbox.md).
add_executable(lua_script_bench lua_script_bench.cpp)
target_link_libraries(
    lua_script_bench
    PRIVATE eawr::script_authoritative eawr::script_numeric eawr::platform eawr::sim eawr::core
)
target_compile_features(lua_script_bench PRIVATE cxx_std_20)
eawr_target_project_warnings(lua_script_bench)
add_test(NAME lua_script_bench_smoke COMMAND lua_script_bench --quick --workers 1,2)

# Canonical Lua persistence and the versioned script state hash (#248,
# docs/lua-persistence.md).
add_executable(lua_persistence_tests lua_persistence_tests.cpp
    lua_persistence_inflated_tests.cpp)
target_link_libraries(
    lua_persistence_tests
    PRIVATE eawr::script_authoritative eawr::script_numeric eawr::platform eawr::sim eawr::core
)
target_compile_features(lua_persistence_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(lua_persistence_tests)

foreach(_eawr_mode IN ITEMS roundtrip workers corruption rejects)
    add_test(NAME lua_persistence_${_eawr_mode} COMMAND lua_persistence_tests ${_eawr_mode})
endforeach()

# Script commands as next-tick replay input (#247, docs/lua-sandbox.md).
add_executable(lua_bridge_tests lua_bridge_tests.cpp)
target_link_libraries(
    lua_bridge_tests
    PRIVATE eawr::script_authoritative eawr::script_numeric eawr::ui_model eawr::platform eawr::sim eawr::core
)
target_compile_features(lua_bridge_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(lua_bridge_tests)

foreach(_eawr_mode IN ITEMS routing workers)
    add_test(NAME lua_bridge_${_eawr_mode} COMMAND lua_bridge_tests ${_eawr_mode})
endforeach()

# Metered pattern matching and the logical memory quota (#375,
# docs/lua-sandbox.md). The pattern modes compare with the P0 runtime
# (upstream Lua 5.0.2); foc_patterns skips unless EAWR_EAW_GAME_ROOT is set.
add_executable(lua_limits_tests lua_limits_tests.cpp "${PROJECT_SOURCE_DIR}/tests/script/numeric/p0_oracle.cpp")
target_include_directories(lua_limits_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/script/numeric")
target_link_libraries(
    lua_limits_tests
    PRIVATE eawr::script_authoritative eawr::script_numeric eawr::lua_502_sf eawr::platform eawr::sim eawr::vfs eawr::core
            eawr::lua_502
)
target_compile_features(lua_limits_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(lua_limits_tests)

foreach(_eawr_mode IN ITEMS metering_width patterns foc_patterns memory memory_persistence memory_workers)
    add_test(NAME lua_limits_${_eawr_mode} COMMAND lua_limits_tests ${_eawr_mode})
endforeach()
set_tests_properties(lua_limits_foc_patterns PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")

# The engine side of the scheduler (#79): handle methods, engine calls and pumps.
add_executable(lua_engine_tests lua_engine_tests.cpp)
target_link_libraries(
    lua_engine_tests
    PRIVATE eawr::script_authoritative eawr::script_numeric eawr::platform eawr::sim eawr::core
)
target_compile_features(lua_engine_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(lua_engine_tests)
add_test(NAME lua_engine_contracts COMMAND lua_engine_tests)

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
