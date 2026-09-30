find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

add_test(
    NAME inventory_contracts
    COMMAND
        "${Python3_EXECUTABLE}" -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}"
        -p "test_*.py"
)
set_tests_properties(
    inventory_contracts
    PROPERTIES
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)

# #628: the M2 scene against the tag registry docs/tag-coverage/statuses.json, and the registry gate against the game
# data (docs/tag-coverage.md); needs EAWR_EAW_GAME_ROOT, skipped otherwise. The registry's own gate runs without
# game data in inventory_contracts (its unit tests) and ci_tag_registry (tests/ci, label ci-tools).
add_test(
    NAME tag_coverage_m2_scene
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_tag_coverage_scene.py"
        --program "$<TARGET_FILE:sim_headless>"
        --work "${CMAKE_CURRENT_BINARY_DIR}/tag-coverage"
)
set_tests_properties(tag_coverage_m2_scene PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")

# The tag perturbation check end to end on four registry rows (docs/tag-applied-check.md); needs
# EAWR_EAW_GAME_ROOT, skipped otherwise. The full check runs nightly or on demand, not here.
add_test(
    NAME tag_applied_check_smoke
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_tag_applied_check.py"
        --binary "$<TARGET_FILE:foc_tag_perturb>"
        --work "${CMAKE_CURRENT_BINARY_DIR}/tag-applied-check"
)
set_tests_properties(tag_applied_check_smoke PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
