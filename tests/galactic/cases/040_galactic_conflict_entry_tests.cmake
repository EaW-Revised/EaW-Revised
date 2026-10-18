eawr_add_test_case(
    NAME galactic_conflict_entry_tests
    SOURCES conflict_entry_tests.cpp
    LIBS eawr::campaign eawr::platform eawr::scene eawr::checkpoint
    TESTS galactic_conflict_entry_contracts
)

add_test(NAME galactic_map_object_admission COMMAND galactic_conflict_entry_tests --map-admission)
add_test(NAME galactic_handoff_replay_corpus COMMAND galactic_conflict_entry_tests --handoff)
add_test(NAME galactic_conflict_entry_cli
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/conflict_entry_cli_check.py"
        --builder "$<TARGET_FILE:galactic_conflict_entry_tests>"
        --program "$<TARGET_FILE:sim_headless>")
set_tests_properties(galactic_conflict_entry_cli PROPERTIES SKIP_RETURN_CODE 77)
