add_executable(unit_scan main.cpp)
target_link_libraries(unit_scan PRIVATE eawr::units)
target_compile_features(unit_scan PRIVATE cxx_std_20)
eawr_target_project_warnings(unit_scan)
