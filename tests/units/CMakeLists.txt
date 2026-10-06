add_executable(unit_tables_tests unit_tables_tests.cpp
    unit_tables_fixture.cpp
    unit_tables_synthetic_tests.cpp
    unit_tables_foc_tests.cpp)
target_link_libraries(unit_tables_tests PRIVATE eawr::units eawr::platform eawr::skirmish eawr::ui_model)
target_compile_definitions(unit_tables_tests PRIVATE EAWR_UNIT_PINNED_REPLAY="${PROJECT_SOURCE_DIR}/tests/skirmish/fixtures/m2-start.eawr-replay")
target_compile_features(unit_tables_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(unit_tables_tests)
add_test(NAME unit_tables_contracts COMMAND unit_tables_tests)

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
