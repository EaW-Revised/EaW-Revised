# The hosted FoC goal-system pieces (#449, docs/behaviour/foc-tactical-ai.md).
add_executable(foc_ai_contract_tests
    foc_ai_contract_tests.cpp
    foc_ai_contract_bindings_tests.cpp
    foc_ai_contract_economy_tests.cpp
    foc_ai_contract_tactical_tests.cpp
    foc_ai_contract_objects_tests.cpp)
target_link_libraries(foc_ai_contract_tests PRIVATE eawr::script_foc eawr::platform eawr::platform_live_ai eawr::vfs)
target_include_directories(foc_ai_contract_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc")
target_compile_features(foc_ai_contract_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(foc_ai_contract_tests)
add_test(NAME foc_ai_contracts COMMAND foc_ai_contract_tests)
add_test(NAME foc_ai_reachable_target COMMAND foc_ai_contract_tests --reachable-target)
add_test(NAME foc_ai_galactic_freestore_stock COMMAND foc_ai_contract_tests --installed-galactic-freestore)
set_tests_properties(foc_ai_galactic_freestore_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME foc_object_scripts COMMAND foc_ai_contract_tests --object-scripts)
add_test(NAME foc_object_scripts_stock COMMAND foc_ai_contract_tests --stock-object-scripts)
set_tests_properties(foc_object_scripts_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")

add_executable(foc_ai_equation_scratch_tests ai_equation_scratch_tests.cpp)
target_link_libraries(foc_ai_equation_scratch_tests PRIVATE eawr::script_foc eawr::platform)
target_include_directories(foc_ai_equation_scratch_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc")
target_compile_features(foc_ai_equation_scratch_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(foc_ai_equation_scratch_tests)
add_test(NAME foc_ai_equation_scratch COMMAND foc_ai_equation_scratch_tests)

add_executable(foc_ai_cell_fog_tests ai_cell_fog_tests.cpp)
target_link_libraries(foc_ai_cell_fog_tests PRIVATE eawr::script_foc eawr::platform)
target_include_directories(foc_ai_cell_fog_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc")
target_compile_features(foc_ai_cell_fog_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(foc_ai_cell_fog_tests)
add_test(NAME foc_ai_cell_fog COMMAND foc_ai_cell_fog_tests)

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
