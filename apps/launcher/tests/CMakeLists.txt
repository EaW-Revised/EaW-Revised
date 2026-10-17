# Launcher tests (#3962): the core against fixtures, and EAWR.exe against a fake Godot.
set(EAWR_LAUNCHER_RUNTIME "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(launcher_tests launcher_tests.cpp)
target_link_libraries(launcher_tests PRIVATE eawr_launcher_core)
set_target_properties(launcher_tests PROPERTIES MSVC_RUNTIME_LIBRARY "${EAWR_LAUNCHER_RUNTIME}")
eawr_target_project_warnings(launcher_tests)
add_test(NAME launcher_core_contracts COMMAND launcher_tests "${PROJECT_SOURCE_DIR}/apps/viewer/project/project.godot")

# Console-subsystem on purpose (see the file header).
add_executable(launcher_fake_godot fake_godot.cpp)
target_link_libraries(launcher_fake_godot PRIVATE eawr_launcher_core)
set_target_properties(launcher_fake_godot PROPERTIES MSVC_RUNTIME_LIBRARY "${EAWR_LAUNCHER_RUNTIME}")
eawr_target_project_warnings(launcher_fake_godot)

add_test(NAME launcher_smoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/launcher_smoke.py"
            "$<TARGET_FILE:eawr_launcher>" "$<TARGET_FILE:launcher_fake_godot>"
            "${PROJECT_SOURCE_DIR}/apps/viewer/project/project.godot")
set_tests_properties(launcher_smoke PROPERTIES TIMEOUT 300)

# The real dialogs, clicked on a private desktop (nothing reaches a screen). Skips (77) when the host
# cannot create one.



eawr_label_tests()
