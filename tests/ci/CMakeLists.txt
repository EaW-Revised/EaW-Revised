find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The XML tag registry gate (docs/tag-coverage.md): every (class, tag) of the FoC XML has a row with what its status
# needs, every applied code location exists, the JSON and its rendering are canonical. Without game data; the
# game-data half (tag_registry_game_data) sits with the inventory tests.
add_test(
    NAME ci_tag_registry
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/inventory/tag_registry.py" check
            --repo "${PROJECT_SOURCE_DIR}"
)

# #527: the shared evidence-read retry guard, and that every tools/compare_*.py comparator uses it.
add_test(
    NAME ci_evidence_io
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_evidence_io.py"
)

# #825: no FoC engine class/method names in tracked files (tools/cleanroom_check.py).
add_test(
    NAME ci_cleanroom_check
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cleanroom_check.py"
)

# The publish scan the public tree must pass (tools/publish_scan; the public CI runs it over the tree).
add_test(
    NAME ci_publish_scan
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_publish_scan.py"
)

# The release packager (tools/release, docs/releasing.md) on a stand-in build tree.
add_test(
    NAME ci_package_release
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_package_release.py"
)

# Every test file above is registered and runs tests (the tools/offload, tools/common and
# tools/rig tests live here, not next to the tools).
add_test(
    NAME ci_registration
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_ci_registration.py"
)

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests(DEFAULT ci-tools)
