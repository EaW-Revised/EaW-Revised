add_executable(eawr_renderer_contracts
    main.cpp
    renderer_resource_tests.cpp
    renderer_submission_tests.cpp
    renderer_surface_tests.cpp
    "${PROJECT_SOURCE_DIR}/src/presentation/godot/renderer_contract.cpp"
)
target_include_directories(eawr_renderer_contracts PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src/presentation/godot"
)
target_link_libraries(eawr_renderer_contracts PRIVATE eawr::core eawr::sim)
target_compile_features(eawr_renderer_contracts PRIVATE cxx_std_20)
eawr_target_project_warnings(eawr_renderer_contracts)
add_test(NAME renderer_contracts COMMAND eawr_renderer_contracts)

# WP-43 legacy/ family adapters (src/presentation/godot/legacy/).
add_executable(eawr_legacy_family_contracts
    legacy_family_tests.cpp
    "${PROJECT_SOURCE_DIR}/src/presentation/godot/renderer_contract.cpp"
)
target_include_directories(eawr_legacy_family_contracts PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src/presentation/godot"
)
target_link_libraries(eawr_legacy_family_contracts PRIVATE eawr::core eawr::sim)
target_compile_features(eawr_legacy_family_contracts PRIVATE cxx_std_20)
eawr_target_project_warnings(eawr_legacy_family_contracts)
add_test(NAME legacy_family_contracts COMMAND eawr_legacy_family_contracts)

# The production GodotRenderer resource-churn probe (tests/presentation/
# renderer/churn) is built separately against the pinned godot-cpp cache; its
# report verifier always runs, its graphical run skips unless
# EAWR_GODOT_RESOURCE_CHURN_RUNTIME_TEST is set.
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME viewer_profile_argument_contracts
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_profile_argument.py"
)
add_test(
    NAME renderer_resource_churn_runtime
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_resource_churn_runtime.py"
)
# The WP-08 renderer fault probe (tests/presentation/renderer/faults) is built
# the same way. Its report and engine-output verifiers always run; the Godot
# runs skip unless EAWR_GODOT_RENDERER_FAULT_RUNTIME_TEST is set.
add_test(
    NAME renderer_fault_runtime
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_renderer_fault_runtime.py"
)

# The WP-43 legacy/ family probe (tests/presentation/renderer/legacy) is built
# the same way; its report verifier always runs, its graphical run skips
# unless EAWR_GODOT_LEGACY_FAMILY_RUNTIME_TEST is set.
add_test(
    NAME legacy_family_runtime
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_legacy_family_runtime.py"
)

add_executable(eawr_map_fog_contracts
    fog_mode_tests.cpp
    "${PROJECT_SOURCE_DIR}/apps/viewer/src/fog_mode.cpp"
)
target_include_directories(eawr_map_fog_contracts PRIVATE
    "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    "${PROJECT_SOURCE_DIR}/include"
)
target_link_libraries(eawr_map_fog_contracts PRIVATE eawr::core eawr::sim)
target_compile_features(eawr_map_fog_contracts PRIVATE cxx_std_20)
target_compile_definitions(eawr_map_fog_contracts PRIVATE
    EAWR_REPLAY_FIXTURE_PATH="${PROJECT_SOURCE_DIR}/tests/replay/fixtures/original-v1.eawr-replay")
eawr_target_project_warnings(eawr_map_fog_contracts)
add_test(NAME map_fog_contracts COMMAND eawr_map_fog_contracts)

add_executable(eawr_render_profile_contracts render_profile_tests.cpp)
target_include_directories(eawr_render_profile_contracts PRIVATE "${PROJECT_SOURCE_DIR}/apps/viewer/src")
target_compile_features(eawr_render_profile_contracts PRIVATE cxx_std_20)
eawr_target_project_warnings(eawr_render_profile_contracts)
add_test(NAME render_profile_contracts COMMAND eawr_render_profile_contracts)

add_executable(eawr_space_fog_contracts
    space_fog_tests.cpp
    "${PROJECT_SOURCE_DIR}/apps/viewer/src/space_fog.cpp"
)
target_include_directories(eawr_space_fog_contracts PRIVATE
    "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    "${PROJECT_SOURCE_DIR}/include"
)
target_link_libraries(eawr_space_fog_contracts PRIVATE eawr::scene eawr::sim eawr::core)
target_compile_features(eawr_space_fog_contracts PRIVATE cxx_std_20)
eawr_target_project_warnings(eawr_space_fog_contracts)
add_test(NAME space_fog_contracts COMMAND eawr_space_fog_contracts)

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
