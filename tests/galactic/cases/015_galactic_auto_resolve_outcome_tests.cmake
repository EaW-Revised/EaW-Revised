eawr_add_test_case(
    NAME galactic_auto_resolve_outcome_tests
    SOURCES auto_resolve_outcome_tests.cpp
    LIBS eawr::campaign eawr::platform eawr::checkpoint
)

add_test(NAME galactic_auto_resolve_outcome COMMAND galactic_auto_resolve_outcome_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/outcome-corpus/space-loss.eawr-gcreplay")
