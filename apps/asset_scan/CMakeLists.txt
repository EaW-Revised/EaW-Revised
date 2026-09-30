add_executable(asset_scan main.cpp)
target_link_libraries(asset_scan PRIVATE eawr::vfs)
target_compile_features(asset_scan PRIVATE cxx_std_20)
eawr_target_project_warnings(asset_scan)
