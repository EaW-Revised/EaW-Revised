add_executable(lighting_tests lighting_tests.cpp)
target_link_libraries(lighting_tests PRIVATE eawr::presentation)
# The frozen P0 hemisphere constants live in the engine-free adapter header.
target_include_directories(lighting_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/presentation/godot")
target_compile_features(lighting_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(lighting_tests)
add_test(NAME lighting_contracts COMMAND lighting_tests)

add_executable(wind_tests wind_tests.cpp)
target_link_libraries(wind_tests PRIVATE eawr::presentation)
target_compile_features(wind_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(wind_tests)
add_test(NAME foliage_wind_contracts COMMAND wind_tests)

add_executable(scene_bloom_tests scene_bloom_tests.cpp)
target_link_libraries(scene_bloom_tests PRIVATE eawr::presentation)
target_compile_features(scene_bloom_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(scene_bloom_tests)
add_test(NAME scene_bloom_contracts COMMAND scene_bloom_tests)

# Read-only generator for plan/inventories/lighting-constants.json; built with
# the tests, run by hand against an installation.
add_executable(lighting_constants_scan constants_scan.cpp)
target_link_libraries(lighting_constants_scan PRIVATE eawr::presentation eawr::assets eawr::vfs eawr::sim)
target_compile_features(lighting_constants_scan PRIVATE cxx_std_20)
eawr_target_project_warnings(lighting_constants_scan)

find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME lighting_constants_contracts
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_lighting_constants.py"
)

add_test(
    NAME lighting_renderer_probe_contracts
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_lighting_renderer_runtime.py"
)

# CPU ray-traced receiver/lit masks for the real MC-50 Hull shadow fixture
# (synthetic geometry by default; the pinned asset only when opted in).
add_executable(hull_shadow_masks_tests
    hull_shadow_masks_tests.cpp
    renderer/hull_shadow_masks.cpp
    renderer/hull_asset_fixture.cpp
    "${PROJECT_SOURCE_DIR}/apps/viewer/src/model_preview.cpp"
)
target_include_directories(hull_shadow_masks_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/renderer" "${PROJECT_SOURCE_DIR}/apps/viewer/src")
target_link_libraries(hull_shadow_masks_tests PRIVATE
    eawr::presentation eawr::assets eawr::vfs eawr::sim)
target_compile_features(hull_shadow_masks_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(hull_shadow_masks_tests)
add_test(NAME hull_shadow_masks_contracts COMMAND hull_shadow_masks_tests)

add_test(
    NAME hull_asset_shadow_probe_contracts
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_hull_asset_shadow_runtime.py"
)

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
