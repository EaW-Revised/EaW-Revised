# The mod core (src/mods) against synthetic fixture trees built in a temporary folder; no game data.
add_executable(mods_tests mods_tests.cpp chain_tests.cpp)
target_link_libraries(mods_tests PRIVATE eawr::mods)
target_compile_features(mods_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(mods_tests)

add_test(NAME mods_contracts COMMAND mods_tests)

add_executable(mods_profile_tests profile_tests.cpp)
target_link_libraries(mods_profile_tests PRIVATE eawr::mods)
target_compile_features(mods_profile_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(mods_profile_tests)
add_test(NAME mods_profile_contracts COMMAND mods_profile_tests)
add_test(NAME mods_profile_symlink COMMAND mods_profile_tests --symlink-only)
set_tests_properties(mods_profile_symlink PROPERTIES SKIP_RETURN_CODE 77)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME mods_profile_cli COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_profile_cli.py" "$<TARGET_FILE:sim_headless>")

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
