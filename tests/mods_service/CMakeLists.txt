# Contract tests of eawr_mods_service (docs/mods-service-protocol.md): every implemented method and error code,
# the line limit, the exit on a closed stdin, the GUI subsystem, and the committed transcripts.
add_test(NAME mods_service_contract
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/mods_service_contract.py"
            "$<TARGET_FILE:eawr_mods_service>")
set_tests_properties(mods_service_contract PROPERTIES TIMEOUT 300)
add_executable(loadcheck_tests loadcheck_tests.cpp "${PROJECT_SOURCE_DIR}/apps/mods_loadcheck/report.cpp")
target_include_directories(loadcheck_tests PRIVATE "${PROJECT_SOURCE_DIR}/apps/mods_loadcheck")
target_link_libraries(loadcheck_tests PRIVATE eawr::core)
eawr_target_project_warnings(loadcheck_tests)
add_test(NAME loadcheck_units COMMAND loadcheck_tests)
add_executable(fake_loadcheck WIN32 fake_loadcheck.cpp)
target_link_libraries(fake_loadcheck PRIVATE shell32)
eawr_target_project_warnings(fake_loadcheck)
add_test(NAME loadcheck_contract COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/loadcheck_contract.py"
    "$<TARGET_FILE:eawr_mods_service>" "$<TARGET_FILE:fake_loadcheck>" "$<TARGET_FILE:eawr_loadcheck>")
set_tests_properties(loadcheck_contract PROPERTIES TIMEOUT 300)


eawr_label_tests()
