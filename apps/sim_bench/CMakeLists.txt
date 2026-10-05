add_executable(sim_bench main.cpp generate.cpp)
target_link_libraries(sim_bench PRIVATE eawr::sim eawr::platform)
target_compile_features(sim_bench PRIVATE cxx_std_20)
eawr_target_project_warnings(sim_bench)
