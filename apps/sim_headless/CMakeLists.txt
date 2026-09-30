add_executable(sim_headless main.cpp scenario.cpp)
target_link_libraries(sim_headless PRIVATE eawr::skirmish eawr::sim eawr::platform)
target_compile_features(sim_headless PRIVATE cxx_std_20)
eawr_target_project_warnings(sim_headless)
