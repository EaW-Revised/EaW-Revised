add_executable(audio_sfx_tests sfx_tests.cpp)
target_link_libraries(audio_sfx_tests PRIVATE eawr::presentation)
target_compile_features(audio_sfx_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(audio_sfx_tests)
add_test(NAME audio_sfx_contracts COMMAND audio_sfx_tests)

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
