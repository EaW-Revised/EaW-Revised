# The EAWRCKP checkpoint container and restore-and-continue (docs/checkpoint-format.md).
add_executable(checkpoint_tests checkpoint_tests.cpp)
target_link_libraries(checkpoint_tests PRIVATE eawr::checkpoint eawr::platform)
target_compile_features(checkpoint_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(checkpoint_tests)
add_test(NAME checkpoint_container_contracts COMMAND checkpoint_tests)

# Restore and continue at three ticks of every tracked tactical replay the replay corpus runs,
# continued on 1, 2, 4 and 8 workers.
add_test(
    NAME checkpoint_restore_continue_corpus
    COMMAND checkpoint_tests --corpus
        "${PROJECT_SOURCE_DIR}/tests/replay/fixtures/tactical-durability.eawr-replay"
        "${PROJECT_SOURCE_DIR}/tests/replay/fixtures/tactical-motion-blocked.eawr-replay"
        "${PROJECT_SOURCE_DIR}/tests/replay/fixtures/tactical-motion-clipped.eawr-replay"
        "${PROJECT_SOURCE_DIR}/tests/replay/fixtures/tactical-motion.eawr-replay"
        "${PROJECT_SOURCE_DIR}/tests/replay/fixtures/tactical-v2.eawr-replay"
        "${PROJECT_SOURCE_DIR}/tests/replay/fixtures/tactical-visibility-frame-0.eawr-replay"
        "${PROJECT_SOURCE_DIR}/tests/replay/fixtures/tactical-visibility-frame-1.eawr-replay"
        "${PROJECT_SOURCE_DIR}/tests/replay/fixtures/tactical-visibility-frame-2.eawr-replay"
        "${PROJECT_SOURCE_DIR}/tests/replay/fixtures/tactical-visibility-frame-3.eawr-replay"
        "${PROJECT_SOURCE_DIR}/tests/replay/fixtures/tactical-visibility-frame-4.eawr-replay"
        "${PROJECT_SOURCE_DIR}/tests/replay/fixtures/tactical-visibility-frame-5.eawr-replay"
        "${PROJECT_SOURCE_DIR}/tests/replay/fixtures/tactical-visibility-frame-6.eawr-replay"
        "${PROJECT_SOURCE_DIR}/tests/replay/fixtures/tactical-visibility-frame-7.eawr-replay"
        "${PROJECT_SOURCE_DIR}/tests/fidelity/fixtures/S-15-duel-damage.eawr-replay"
        "${PROJECT_SOURCE_DIR}/tests/fidelity/fixtures/S-28-squadron-vs-capital.eawr-replay"
        "${PROJECT_SOURCE_DIR}/tests/fidelity/fixtures/S-51-mc80-ion-drains-tartan.eawr-replay"
        "${PROJECT_SOURCE_DIR}/tests/skirmish/fixtures/m2-start.eawr-replay"
)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME checkpoint_field_completeness
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_fields.py" --check)
add_test(NAME checkpoint_field_completeness_contracts
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_generate_fields.py")

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
