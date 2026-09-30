add_executable(vfs_tests main.cpp)
target_link_libraries(vfs_tests PRIVATE eawr::vfs)
target_compile_features(vfs_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(vfs_tests)

add_test(NAME vfs_contracts COMMAND vfs_tests)

find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME vfs_supplemental
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/acceptance/run_supplemental.py"
        --repo-root "${PROJECT_SOURCE_DIR}"
        --asset-scan "$<TARGET_FILE:asset_scan>"
        --vfs-tests "$<TARGET_FILE:vfs_tests>"
        --synthetic-only
)
add_test(
    NAME asset_io_boundary
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_asset_io_boundary.py"
        --root "${PROJECT_SOURCE_DIR}"
)

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
