add_executable(audio_sfx_tests sfx_tests.cpp)
target_link_libraries(audio_sfx_tests PRIVATE eawr::presentation)
target_compile_features(audio_sfx_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(audio_sfx_tests)
add_test(NAME audio_sfx_contracts COMMAND audio_sfx_tests)

add_executable(audio_ability_history_tests ability_history_tests.cpp)
target_link_libraries(audio_ability_history_tests PRIVATE eawr::presentation)
target_compile_features(audio_ability_history_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(audio_ability_history_tests)
add_test(NAME audio_ability_history_contracts COMMAND audio_ability_history_tests)

add_executable(audio_last_seen_walk_tests last_seen_walk_tests.cpp)
target_link_libraries(audio_last_seen_walk_tests PRIVATE eawr::presentation eawr::core)
target_compile_features(audio_last_seen_walk_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(audio_last_seen_walk_tests)
add_test(NAME audio_last_seen_walk_contracts COMMAND audio_last_seen_walk_tests)

# #1708: sample path forms and WAVE formats on a real VFS; with EAWR_EAW_GAME_ROOT (and
# EAWR_MOD_AUDIO_ROOTS) the installed FoC and mod samples are counted too.
add_executable(audio_sample_corpus_tests sample_corpus_tests.cpp)
target_link_libraries(audio_sample_corpus_tests PRIVATE eawr::presentation eawr::data eawr::vfs eawr::core)
target_compile_features(audio_sample_corpus_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(audio_sample_corpus_tests)
add_test(NAME audio_sample_corpus COMMAND audio_sample_corpus_tests)

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
