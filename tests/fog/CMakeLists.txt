find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

add_executable(fog_contract_tests fog_tests.cpp)
target_link_libraries(fog_contract_tests PRIVATE eawr::sim eawr::platform)
target_compile_features(fog_contract_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(fog_contract_tests)

set(EAWR_FOG_FIXTURES "${CMAKE_CURRENT_LIST_DIR}/fixtures")
set(EAWR_FOG_REPLAY_FIXTURES "${CMAKE_CURRENT_LIST_DIR}/../replay/fixtures")
add_test(
    NAME fog_stub_contracts
    COMMAND fog_contract_tests "${EAWR_FOG_FIXTURES}" "${EAWR_FOG_REPLAY_FIXTURES}"
)
add_test(
    NAME fog_stub_oracle_fixtures
    COMMAND "${Python3_EXECUTABLE}" "${EAWR_FOG_FIXTURES}/generate_fog_fixture.py" --check
)
add_test(
    NAME fog_evidence_comparator_contracts
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_compare_fog_evidence.py"
        --tool "${CMAKE_CURRENT_LIST_DIR}/../../tools/compare_fog_evidence.py"
)

# Separate fog-stub-v1 evidence artifact; replay_evidence and its outputs are untouched.
# The executable also records replay.sha256 and sidecar.sha256 beside these rows.
set(EAWR_FOG_EVIDENCE_DIR "${CMAKE_BINARY_DIR}/fog-evidence")
add_custom_target(
    fog_evidence
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${EAWR_FOG_EVIDENCE_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${EAWR_FOG_EVIDENCE_DIR}"
    COMMAND
        "$<TARGET_FILE:fog_contract_tests>" --evidence
        "${EAWR_FOG_REPLAY_FIXTURES}/original-v1.eawr-replay"
        "${EAWR_FOG_FIXTURES}/fog-stub-v1.eawr-fog"
        "${EAWR_FOG_EVIDENCE_DIR}/workers-1.csv" 1
    COMMAND
        "$<TARGET_FILE:fog_contract_tests>" --evidence
        "${EAWR_FOG_REPLAY_FIXTURES}/original-v1.eawr-replay"
        "${EAWR_FOG_FIXTURES}/fog-stub-v1.eawr-fog"
        "${EAWR_FOG_EVIDENCE_DIR}/workers-2.csv" 2
    COMMAND
        "$<TARGET_FILE:fog_contract_tests>" --evidence
        "${EAWR_FOG_REPLAY_FIXTURES}/original-v1.eawr-replay"
        "${EAWR_FOG_FIXTURES}/fog-stub-v1.eawr-fog"
        "${EAWR_FOG_EVIDENCE_DIR}/workers-4.csv" 4
    DEPENDS fog_contract_tests
    VERBATIM
)

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
