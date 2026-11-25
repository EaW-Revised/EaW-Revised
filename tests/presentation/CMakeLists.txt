# Python source and synthetic evidence contracts.

add_executable(console_core_tests ../console/core_tests.cpp)
target_link_libraries(console_core_tests PRIVATE eawr::presentation eawr::ui_data)
target_compile_features(console_core_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(console_core_tests)
add_test(NAME console_core_contracts COMMAND console_core_tests)
eawr_add_python_contract(python_console_registry tests.console.test_registry)
add_test(NAME console_transcript COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/console/test_transcript.py" "$<TARGET_FILE:sim_headless>")

add_executable(shader_translation_cache_tests shader_translation_cache_tests.cpp)
target_link_libraries(shader_translation_cache_tests PRIVATE eawr::presentation eawr::assets)
target_compile_features(shader_translation_cache_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(shader_translation_cache_tests)
add_test(NAME shader_translation_cache_contracts COMMAND shader_translation_cache_tests)

# Each file runs its CPU contracts; graphical exercises retain their opt-in gates.
add_executable(shader_translation_report_tests shader_translation_report_tests.cpp)
target_link_libraries(shader_translation_report_tests PRIVATE eawr::presentation)
target_compile_features(shader_translation_report_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(shader_translation_report_tests)
add_test(NAME shader_translation_report_contracts COMMAND shader_translation_report_tests)

# --eawr-perf-movers selection (apps/viewer/src/perf_movers.hpp, header only).
add_executable(perf_movers_tests perf_movers_tests.cpp)
target_include_directories(perf_movers_tests PRIVATE "${PROJECT_SOURCE_DIR}/apps/viewer/src")
target_compile_features(perf_movers_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(perf_movers_tests)
add_test(NAME perf_movers_contracts COMMAND perf_movers_tests)

eawr_add_python_contract(python_presentation_gpu_expectations tests.presentation.test_gpu_expectations)
add_subdirectory(shader_runtime)


eawr_label_tests()
