# The mod core (src/mods) against synthetic fixture trees built in a temporary folder; no game data.
add_executable(mods_tests mods_tests.cpp chain_tests.cpp)
target_link_libraries(mods_tests PRIVATE eawr::mods)
target_compile_features(mods_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(mods_tests)

add_test(NAME mods_contracts COMMAND mods_tests)

add_executable(mods_save_description_tests save_description_tests.cpp)
target_link_libraries(mods_save_description_tests PRIVATE eawr::mods)
target_compile_features(mods_save_description_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(mods_save_description_tests)
add_test(NAME mods_save_description_contracts COMMAND mods_save_description_tests)

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
add_executable(mods_file_index_tests file_index_tests.cpp)
target_link_libraries(mods_file_index_tests PRIVATE eawr::mods eawr::vfs)
eawr_target_project_warnings(mods_file_index_tests)
add_test(NAME mods_file_index_contracts COMMAND mods_file_index_tests)

add_executable(mods_shader_translation_tests shader_translation_tests.cpp)
target_link_libraries(mods_shader_translation_tests PRIVATE eawr::mods)
target_compile_features(mods_shader_translation_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(mods_shader_translation_tests)
add_test(NAME mods_shader_translation_contracts COMMAND mods_shader_translation_tests)

add_executable(mods_content_digest_tests content_digest_tests.cpp)
target_link_libraries(mods_content_digest_tests PRIVATE eawr::mods)
target_compile_features(mods_content_digest_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(mods_content_digest_tests)
target_compile_definitions(mods_content_digest_tests PRIVATE EAWR_CONTENT_DIGEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME mods_content_digest_contracts COMMAND mods_content_digest_tests)
set_tests_properties(mods_content_digest_contracts PROPERTIES TIMEOUT 300)

# Measurement tool, not part of the normal build: cmake --build <dir> --target mods_content_digest_tool
# then: mods_content_digest_tool <mod folder> [cache folder] (reads the folder, writes only the cache folder).
add_executable(mods_content_digest_tool EXCLUDE_FROM_ALL content_digest_tool.cpp)
target_link_libraries(mods_content_digest_tool PRIVATE eawr::mods)
target_compile_features(mods_content_digest_tool PRIVATE cxx_std_20)
eawr_target_project_warnings(mods_content_digest_tool)

eawr_label_tests()
