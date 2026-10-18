eawr_add_test_case(
    NAME galactic_audio_tests
    SOURCES audio_tests.cpp
    LIBS eawr::presentation
    TESTS galactic_audio_contracts
)
