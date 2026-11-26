eawr_add_test_case(
    NAME galactic_native_queue_order_tests
    SOURCES native_queue_order_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/sim/galactic"
    LIBS eawr::sim_galactic
    TESTS galactic_ai_native_queue_order_contracts
)

# WGA-509: a planet's AI build queue starts the least-slack waiting AI entry, not the first arrival.
