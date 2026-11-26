eawr_add_test_case(
    NAME galactic_contrast_total_tests
    SOURCES galactic_contrast_total_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::script_foc
    TESTS galactic_ai_contrast_total_contracts
)

# WGA-508: a plan whose attempted domain leaves more than the contrast limit of the target's total force is refused.
