# Called in each relevant test directory after its tests are registered.  The
# environment belongs to the selected tests, not CTest or performance runs.
function(eawr_verify_script_hash_reuse_tests)
    get_property(local_tests DIRECTORY PROPERTY TESTS)
    foreach(test IN LISTS local_tests)
        get_property(test_labels TEST "${test}" PROPERTY LABELS)
        if(test MATCHES "(bench|perf|timing)" OR "benchmark" IN_LIST test_labels)
            continue()
        endif()
        if(ARGV0 STREQUAL "ALL" OR test MATCHES "(galactic|campaign|script|replay|lockstep|desync)")
            set_property(TEST "${test}" APPEND PROPERTY ENVIRONMENT "EAWR_VERIFY_SCRIPT_HASH_REUSE=1")
        endif()
    endforeach()
endfunction()
