add_executable(fog_presentation_tests fog_tests.cpp)
target_link_libraries(fog_presentation_tests PRIVATE eawr::presentation eawr::sim)
target_compile_features(fog_presentation_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(fog_presentation_tests)
add_test(NAME fog_presentation_contracts COMMAND fog_presentation_tests)

# The Godot texture/readback exercise needs the pinned editor binary and a
# separately built probe extension (tests/presentation/fog/godot); it skips
# unless EAWR_GODOT_FOG_RUNTIME_TEST is set.
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME fog_godot_runtime
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_fog_godot_runtime.py"
)

# The production GodotRenderer fog-stub-v1 binding probe (tests/presentation/
# fog/renderer) is built separately against the pinned godot-cpp cache; its
# graphical run skips unless EAWR_GODOT_FOG_RENDERER_RUNTIME_TEST is set.
add_test(
    NAME fog_renderer_godot_runtime
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_fog_renderer_runtime.py"
)

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
