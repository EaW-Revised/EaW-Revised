add_executable(scene_scan main.cpp)
target_link_libraries(scene_scan PRIVATE eawr::scene)
target_compile_features(scene_scan PRIVATE cxx_std_20)
eawr_target_project_warnings(scene_scan)
