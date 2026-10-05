# Run source contracts from the repository root, including split unittest
# modules and scripts whose entry point assembles several test classes.
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
function(eawr_add_python_contract name module)
    cmake_parse_arguments(PARSE_ARGV 2 arg "ENTRYPOINT" "" "NEEDS")
    set(entrypoint "")
    if(arg_ENTRYPOINT)
        set(entrypoint --entrypoint)
    endif()
    add_test(NAME ${name}
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/ci/run_module_tests.py"
                ${entrypoint} --needs ${arg_NEEDS} ${module}
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
endfunction()
