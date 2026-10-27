add_executable(asset_validate main.cpp report.cpp validation.cpp texture_closure.cpp)
target_link_libraries(asset_validate PRIVATE eawr::core eawr::assets eawr::presentation eawr::vfs eawr::scene eawr::skirmish)
target_compile_features(asset_validate PRIVATE cxx_std_20)
eawr_target_project_warnings(asset_validate)
