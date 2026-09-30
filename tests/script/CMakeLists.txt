add_executable(script_tests main.cpp)
target_link_libraries(script_tests PRIVATE eawr::script eawr::vfs)
target_compile_features(script_tests PRIVATE cxx_std_20)
target_compile_definitions(
    script_tests
    PRIVATE EAWR_SCRIPT_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
eawr_target_project_warnings(script_tests)

add_test(NAME script_contracts COMMAND script_tests)
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME sflua_namespace_qualification
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_sflua_namespace.py"
)
add_subdirectory(numeric)
add_subdirectory(authoritative)
add_subdirectory(foc)

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
