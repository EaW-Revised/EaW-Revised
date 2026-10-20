# #4058: checkpoint section sizes of an AI-on campaign over play time and restore-continue equality.
eawr_add_galactic_area_case(
    AREA galactic_area_ai_observation

    NAME galactic_ai_save_growth_tests
    SOURCES ai_save_growth_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::checkpoint eawr::platform
)

foreach(workers 1 4)
    eawr_add_game_data_test(NAME galactic_ai_save_growth_w${workers} COMMAND galactic_area_ai_observation galactic_ai_save_growth_tests 3000 1000 ${workers} 1500)
    set_tests_properties(galactic_ai_save_growth_w${workers} PROPERTIES LABELS "game-data;observation" TIMEOUT 7200 RUN_SERIAL TRUE
        ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
endforeach()

# #4058: a save holds the play since the previous save or load (SaveRequest -> save -> load, three cycles).
eawr_add_game_data_test(NAME galactic_ai_save_cycles_w4 COMMAND galactic_area_ai_observation galactic_ai_save_growth_tests 1500 0 4 0 run - 1500 200)
set_tests_properties(galactic_ai_save_cycles_w4 PROPERTIES LABELS "game-data;observation" TIMEOUT 7200 RUN_SERIAL TRUE
    ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
