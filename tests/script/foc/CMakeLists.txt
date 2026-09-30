# The hosted FoC goal-system pieces (#449, docs/behaviour/foc-tactical-ai.md).
add_executable(foc_ai_contract_tests foc_ai_contract_tests.cpp)
target_link_libraries(foc_ai_contract_tests PRIVATE eawr::script_foc eawr::platform)
target_include_directories(foc_ai_contract_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/script/foc")
target_compile_features(foc_ai_contract_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(foc_ai_contract_tests)
add_test(NAME foc_ai_contracts COMMAND foc_ai_contract_tests)

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
