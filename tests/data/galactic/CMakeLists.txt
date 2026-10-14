# Galactic conquest data loaders (docs/galactic-data.md). Synthetic data always; the stock FoC
# campaigns too when EAWR_EAW_GAME_ROOT is set.

add_executable(galactic_data_tests galactic_data_tests.cpp)
target_link_libraries(galactic_data_tests PRIVATE eawr::data eawr::vfs)
target_compile_features(galactic_data_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(galactic_data_tests)
add_test(NAME galactic_data_contracts COMMAND galactic_data_tests)

eawr_label_tests()
