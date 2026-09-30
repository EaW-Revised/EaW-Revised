add_executable(xml_scan main.cpp)
target_link_libraries(xml_scan PRIVATE eawr::data eawr::vfs)
target_compile_features(xml_scan PRIVATE cxx_std_20)
eawr_target_project_warnings(xml_scan)
