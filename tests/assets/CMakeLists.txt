add_executable(asset_tests main.cpp)
target_link_libraries(asset_tests PRIVATE eawr::assets)
target_compile_features(asset_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(asset_tests)

add_test(NAME asset_contracts COMMAND asset_tests)

add_executable(map_tests map_decode_tests.cpp map_reference_tests.cpp map_tests.cpp)
target_link_libraries(map_tests PRIVATE eawr::assets)
target_compile_features(map_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(map_tests)
add_test(NAME map_contracts COMMAND map_tests)

add_executable(mtd_tests mtd_tests.cpp)
target_link_libraries(mtd_tests PRIVATE eawr::assets)
target_compile_features(mtd_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(mtd_tests)

add_test(NAME mtd_contracts COMMAND mtd_tests)

add_executable(mod_atlas_tests mod_atlas_tests.cpp)
target_link_libraries(mod_atlas_tests PRIVATE eawr::assets)
target_compile_features(mod_atlas_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(mod_atlas_tests)
add_test(NAME mod_atlas_contracts COMMAND mod_atlas_tests)

find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME asset_validate_report_contracts
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_asset_validate.py"
        "--validator" "$<TARGET_FILE:asset_validate>"
)
add_test(
    NAME mtd_inventory_contracts
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_mtd_inventory.py"
)
add_test(
    NAME map_reference_map_pins
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_map_reference_maps.py"
)
add_test(
    NAME sky_scan_report_contracts
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_sky_scan.py" "--scanner" "$<TARGET_FILE:sky_scan>"
)

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
