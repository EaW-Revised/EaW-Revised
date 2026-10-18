# #4058: checkpoint section sizes of an AI-on campaign over play time and restore-continue equality.
eawr_add_test_case(
    NAME galactic_ai_save_growth_tests
    SOURCES ai_save_growth_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::checkpoint eawr::platform
)

foreach(workers 1 4)
    eawr_add_game_data_test(NAME galactic_ai_save_growth_w${workers} COMMAND galactic_ai_save_growth_tests 3000 1000 ${workers} 1500)
    set_tests_properties(galactic_ai_save_growth_w${workers} PROPERTIES LABELS "game-data;observation" TIMEOUT 7200 RUN_SERIAL TRUE
        ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
endforeach()
