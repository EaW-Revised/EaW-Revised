add_executable(path_bench main.cpp melee.cpp sampler.cpp
    path_bench_options.cpp path_bench_scenarios.cpp path_bench_report.cpp)
target_link_libraries(path_bench PRIVATE eawr::skirmish eawr::platform)
if(WIN32)
    target_link_libraries(path_bench PRIVATE dbghelp)
endif()
target_compile_features(path_bench PRIVATE cxx_std_20)
eawr_target_project_warnings(path_bench)
