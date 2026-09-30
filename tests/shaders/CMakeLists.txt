find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

# Portable synthetic parser, lowering, policy and corpus-contract coverage. The
# private Petroglyph corpus is not required in CI; tests that require the optional
# pinned compiler/validator installation report themselves as skipped.
add_test(
    NAME shader_synthetic_contracts
    COMMAND
        "${Python3_EXECUTABLE}" -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}" -p "test_*.py" -v
)
set_tests_properties(
    shader_synthetic_contracts
    PROPERTIES
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        LABELS "shaders;portable;synthetic"
)

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
