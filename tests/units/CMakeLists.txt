add_executable(unit_tables_tests unit_tables_tests.cpp
    unit_tables_fixture.cpp
    unit_tables_synthetic_tests.cpp
    unit_tables_synthetic_weapons_tests.cpp
    unit_tables_ability_limits_tests.cpp
    unit_tables_synthetic_loader_tests.cpp
    unit_tables_synthetic_identity_tests.cpp
    unit_tables_domain_tests.cpp
    unit_tables_land_tests.cpp
    unit_tables_foc_tests.cpp
    unit_tables_foc_defence_tests.cpp
    unit_tables_foc_weapons_tests.cpp
    unit_tables_foc_world_tests.cpp)
target_link_libraries(unit_tables_tests PRIVATE eawr::units eawr::platform_live_ai eawr::skirmish eawr::ui_model)
target_compile_definitions(unit_tables_tests PRIVATE EAWR_UNIT_PINNED_REPLAY="${PROJECT_SOURCE_DIR}/tests/skirmish/fixtures/m2-start.eawr-replay")
target_compile_features(unit_tables_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(unit_tables_tests)
add_test(NAME unit_tables_contracts COMMAND unit_tables_tests)
add_test(NAME unit_mod_loadability_contracts COMMAND unit_tables_tests --mod-loadability)
add_test(NAME unit_land_static_contracts COMMAND unit_tables_tests --land-companies)
add_test(NAME unit_remake_combat_guard COMMAND unit_tables_tests --remake-combat-guard)
set_tests_properties(unit_remake_combat_guard PROPERTIES SKIP_RETURN_CODE 77)
eawr_add_game_data_test(NAME unit_tables_installed_contracts COMMAND unit_tables_tests --installed)
eawr_add_game_data_test(NAME unit_fire_bones_installed COMMAND unit_tables_tests --fire-bones-installed)
add_test(NAME unit_space_population COMMAND unit_tables_tests --space-population)
add_test(NAME unit_formation_deceleration_contracts COMMAND unit_tables_tests --formation-deceleration)
eawr_add_game_data_test(NAME unit_formation_deceleration_installed_contracts
    COMMAND unit_tables_tests --formation-deceleration-installed)
add_test(NAME unit_object_script_resolution COMMAND unit_tables_tests --object-scripts)

add_executable(unit_tables_retail_int_tests unit_tables_retail_int_tests.cpp unit_tables_fixture.cpp)
target_link_libraries(unit_tables_retail_int_tests PRIVATE eawr::units)
target_compile_features(unit_tables_retail_int_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(unit_tables_retail_int_tests)
add_test(NAME unit_tables_retail_int_contracts COMMAND unit_tables_retail_int_tests)

add_executable(galactic_types_tests galactic_types_tests.cpp)
target_link_libraries(galactic_types_tests PRIVATE eawr::units)
target_compile_features(galactic_types_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_types_tests)
add_test(NAME galactic_type_contracts COMMAND galactic_types_tests)
eawr_add_game_data_test(NAME galactic_type_installed_contracts COMMAND galactic_types_tests --installed)

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
