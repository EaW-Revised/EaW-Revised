eawr_add_test_case(
    NAME galactic_ai_lifecycle_tests
    SOURCES ai_lifecycle_stock_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::script_foc eawr::script_galactic eawr::platform eawr::vfs
    TESTS galactic_ai_lifecycle_contracts
)

eawr_add_game_data_test(NAME galactic_ai_lifecycle_loading_stock COMMAND galactic_ai_lifecycle_tests --installed)
set_tests_properties(galactic_ai_lifecycle_loading_stock PROPERTIES LABELS game-data)
add_test(NAME galactic_ai_lifecycle_state_contracts COMMAND galactic_ai_lifecycle_tests --state)
foreach(workers IN ITEMS 1 2 4 8)
    add_test(NAME galactic_ai_bounded_diagnostics_${workers} COMMAND galactic_ai_lifecycle_tests --bounded ${workers})
endforeach()
# Shared immutable per-player policy snapshots: zero copies while unchanged, one per write/restore.
