# #4058: a GC save keeps the history since the previous save or load; the input frontier chains through it.
eawr_add_galactic_area_case(
    AREA galactic_area_checkpoint

    NAME galactic_bounded_history_tests
    SOURCES bounded_history_tests.cpp
    LIBS eawr::checkpoint eawr::platform
)

add_test(NAME galactic_bounded_history_contracts COMMAND galactic_area_checkpoint galactic_bounded_history_tests)
