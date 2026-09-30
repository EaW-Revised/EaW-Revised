add_executable(unit_tables_tests unit_tables_tests.cpp)
target_link_libraries(unit_tables_tests PRIVATE eawr::units)
target_compile_features(unit_tables_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(unit_tables_tests)
add_test(NAME unit_tables_contracts COMMAND unit_tables_tests)

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
