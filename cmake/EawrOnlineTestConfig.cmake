# Configure and standalone packaging enforce the native options.cpp host/trust rules.
# Both validators use the host's inet_pton and the same ASCII DNS label limits.
function(eawr_validate_online_test_config address port trust)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    execute_process(COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../apps/online_test/validate_package_config.py"
        "${address}" "${port}" "${trust}"
        RESULT_VARIABLE _validation_result ERROR_VARIABLE _validation_error TIMEOUT 10)
    if(NOT _validation_result EQUAL 0)
        message(FATAL_ERROR "Online test package: ${_validation_error}")
    endif()
endfunction()
