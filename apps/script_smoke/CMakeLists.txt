add_executable(script_smoke main.cpp)
target_link_libraries(script_smoke PRIVATE eawr::script eawr::vfs)
target_compile_features(script_smoke PRIVATE cxx_std_20)
eawr_target_project_warnings(script_smoke)
