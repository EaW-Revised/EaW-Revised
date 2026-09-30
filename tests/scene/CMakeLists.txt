add_executable(scene_tests scene_asset_state_tests.cpp scene_build_tests.cpp scene_issue_tests.cpp scene_tests.cpp)
target_link_libraries(scene_tests PRIVATE eawr::scene eawr::platform)
target_compile_features(scene_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(scene_tests)
add_test(NAME scene_contracts COMMAND scene_tests)

add_executable(space_population_tests space_population_tests.cpp)
target_link_libraries(space_population_tests PRIVATE eawr::scene)
target_compile_features(space_population_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(space_population_tests)
add_test(NAME space_population_contracts COMMAND space_population_tests)

add_executable(idle_tags_tests idle_tags_tests.cpp)
target_link_libraries(idle_tags_tests PRIVATE eawr::scene)
target_compile_features(idle_tags_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(idle_tags_tests)
add_test(NAME idle_tags_contracts COMMAND idle_tags_tests)

find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME scene_inventory_contracts
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_scene_inventory.py"
        --scanner "$<TARGET_FILE:scene_scan>"
)

add_executable(scene_evidence_producer scene_evidence.cpp)
target_link_libraries(scene_evidence_producer PRIVATE eawr::scene eawr::platform)
target_compile_features(scene_evidence_producer PRIVATE cxx_std_20)
eawr_target_project_warnings(scene_evidence_producer)

set(EAWR_SCENE_FIXTURE "${PROJECT_SOURCE_DIR}/tests/assets/fixtures/scene_fixture.py")
set(EAWR_SCENE_EVIDENCE_DIR "${CMAKE_BINARY_DIR}/scene-evidence")
add_custom_target(scene_evidence
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/produce_scene_evidence.py"
        --program "$<TARGET_FILE:scene_evidence_producer>"
        --fixture "${EAWR_SCENE_FIXTURE}"
        --output "${EAWR_SCENE_EVIDENCE_DIR}"
    DEPENDS scene_evidence_producer
    VERBATIM
)

add_test(NAME scene_evidence_contracts
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/produce_scene_evidence.py"
        --program "$<TARGET_FILE:scene_evidence_producer>"
        --fixture "${EAWR_SCENE_FIXTURE}"
        --output "${CMAKE_BINARY_DIR}/scene-evidence-test"
        --target local-test
        --build-id 0000000000000000000000000000000000000000
        --self-test
)
add_test(NAME scene_evidence_comparator_contracts
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}" -p test_compare_scene_evidence.py
)
set_tests_properties(scene_evidence_comparator_contracts PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
