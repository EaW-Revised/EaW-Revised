add_executable(sim_headless main.cpp cli_options.cpp cli_replay.cpp cli_skirmish.cpp cli_report.cpp
    scenario.cpp scenario_build.cpp scenario_run.cpp scenario_report.cpp)
target_link_libraries(sim_headless PRIVATE eawr::skirmish eawr::sim eawr::platform)
target_compile_features(sim_headless PRIVATE cxx_std_20)
eawr_target_project_warnings(sim_headless)
