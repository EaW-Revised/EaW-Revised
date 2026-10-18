# Shared defaults for engine-free C++ test executables. Argument-bearing tests,
# game-data wrappers and per-test properties stay explicit in each case fragment.
include_guard(GLOBAL)

function(eawr_add_test_case)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "NAME" "SOURCES;LIBS;INCLUDE_DIRS;TESTS")
    if(arg_UNPARSED_ARGUMENTS OR arg_KEYWORDS_MISSING_VALUES OR NOT arg_NAME OR NOT arg_SOURCES)
        message(FATAL_ERROR "eawr_add_test_case requires NAME and SOURCES, with optional LIBS, INCLUDE_DIRS and TESTS")
    endif()

    add_executable(${arg_NAME} ${arg_SOURCES})
    if(arg_INCLUDE_DIRS)
        target_include_directories(${arg_NAME} PRIVATE ${arg_INCLUDE_DIRS})
    endif()
    if(arg_LIBS)
        target_link_libraries(${arg_NAME} PRIVATE ${arg_LIBS})
    endif()
    target_compile_features(${arg_NAME} PRIVATE cxx_std_20)
    eawr_target_project_warnings(${arg_NAME})

    foreach(test IN LISTS arg_TESTS)
        add_test(NAME ${test} COMMAND ${arg_NAME})
    endforeach()
endfunction()
