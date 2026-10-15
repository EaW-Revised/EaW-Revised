# Online test kit (#3646): parsing/sanitizer units and a loopback host+join run against the local relay.
add_executable(online_test_unit_tests online_test_unit_tests.cpp)
target_include_directories(online_test_unit_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/net")
target_link_libraries(online_test_unit_tests PRIVATE eawr_online_test_core eawr::net)
eawr_target_project_warnings(online_test_unit_tests)
add_test(NAME online_test_unit_contracts COMMAND online_test_unit_tests)

# The process harness launches the tool and the relay built from this same checkout.
add_dependencies(online_test_unit_tests eawr_online_test eawr_relay_server)
add_test(NAME online_test_loopback COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/loopback.py"
    "$<TARGET_FILE:eawr_online_test>" "$<TARGET_FILE:eawr_relay_server>" "${PROJECT_BINARY_DIR}/online-test-artifacts")
set_tests_properties(online_test_loopback PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
add_test(NAME online_test_pin_loopback COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/loopback.py"
    "$<TARGET_FILE:eawr_online_test>" "$<TARGET_FILE:eawr_relay_server>" "${PROJECT_BINARY_DIR}/online-test-pin-artifacts" pin)
set_tests_properties(online_test_pin_loopback PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
add_test(NAME online_test_package_import_contracts COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/package_import_tests.py")

# The zip testers get: built by the packaging script, contents and launchers checked, and the
# packaged exe run once against a closed port. Left at <build>/online-test-package/EAWR-online-test.zip.
add_test(NAME online_test_package COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify_package.py"
    "${CMAKE_COMMAND}" "$<TARGET_FILE:eawr_online_test>" "${PROJECT_SOURCE_DIR}/apps/online_test/package"
    "${PROJECT_SOURCE_DIR}/cmake/EawrOnlineTestPackage.cmake" "${PROJECT_BINARY_DIR}/online-test-package"
    "${EAWR_ONLINE_TEST_RUNTIME_DLLS}")
set_tests_properties(online_test_package PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 150)
eawr_label_tests()
