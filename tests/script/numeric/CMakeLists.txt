# Authoritative Lua numeric profile (#246). Expected digests come from
# sequence-manifest.json, generated offline by
# tools/generate_lua_numeric_vectors.py from hardware binary64.
add_executable(lua_numeric_tests lua_numeric_tests.cpp p0_oracle.cpp)
target_link_libraries(lua_numeric_tests PRIVATE eawr::lua_502_sf eawr::script_numeric eawr::core eawr::lua_502)
target_compile_features(lua_numeric_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(lua_numeric_tests)

add_test(
    NAME lua_numeric_vectors
    COMMAND lua_numeric_tests vectors "${CMAKE_CURRENT_SOURCE_DIR}/vectors.txt"
)
add_test(NAME lua_numeric_vm COMMAND lua_numeric_tests vm)
# The #376 rig probe: the retail FoC line recorded in foc-retail-probe.txt.
add_test(
    NAME lua_numeric_foc_retail_probe
    COMMAND
        lua_numeric_tests foc-probe "${PROJECT_SOURCE_DIR}/tools/validation/p1_capture/lua_number_probe.lua"
        "${CMAKE_CURRENT_SOURCE_DIR}/foc-retail-probe.txt"
)
# The #351/#81 staging probe through two battles and refused engine calls (fake engine).
add_test(
    NAME lua_staging_probe_battles
    COMMAND
        lua_numeric_tests script "${PROJECT_SOURCE_DIR}/tools/validation/p1_capture/staging_probe.lua"
        "${PROJECT_SOURCE_DIR}/tests/validation/p1_capture/staging_probe_battles.lua"
)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/sequence-manifest.json" _eawr_numeric_manifest)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/sequence-manifest.json")
set(_eawr_numeric_sequence_args)
foreach(_eawr_sequence IN ITEMS arithmetic compare integer q24 parse format)
    string(JSON _eawr_digest GET "${_eawr_numeric_manifest}" sequences ${_eawr_sequence} sha256)
    list(APPEND _eawr_numeric_sequence_args "--${_eawr_sequence}" "${_eawr_digest}")
endforeach()
add_test(NAME lua_numeric_sequences COMMAND lua_numeric_tests sequences ${_eawr_numeric_sequence_args})

# Load-budget benchmark: the soft-float VM against a benchmark-only
# hardware-double twin of the same upstream sources (never authoritative).
eawr_add_sflua_library(eawr_lua_502_hw_bench)
target_compile_definitions(eawr_lua_502_hw_bench PUBLIC EAWR_SFLUA_HARDWARE_TWIN=1)
add_library(eawr_lua_numeric_bench_sf OBJECT bench/bench_vm.cpp)
target_link_libraries(eawr_lua_numeric_bench_sf PRIVATE eawr::lua_502_sf)
add_library(eawr_lua_numeric_bench_hw OBJECT bench/bench_vm.cpp)
target_link_libraries(eawr_lua_numeric_bench_hw PRIVATE eawr_lua_502_hw_bench)
add_executable(lua_numeric_bench bench/lua_numeric_bench.cpp)
target_link_libraries(
    lua_numeric_bench
    PRIVATE
        eawr_lua_numeric_bench_sf eawr_lua_numeric_bench_hw eawr::lua_502_sf eawr_lua_502_hw_bench
        eawr::script_numeric
)
target_compile_features(lua_numeric_bench PRIVATE cxx_std_20)
add_test(NAME lua_numeric_bench_smoke COMMAND lua_numeric_bench --quick)

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
