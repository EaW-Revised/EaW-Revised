eawr_add_galactic_area_case(
    AREA galactic_area_checkpoint

    NAME galactic_production_tests
    SOURCES production_tests.cpp
        production_queue.cpp
        production_state.cpp
    LIBS eawr::checkpoint eawr::platform
)

add_test(NAME galactic_production_contracts COMMAND galactic_area_checkpoint galactic_production_tests "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_test(NAME galactic_production_offer_contracts COMMAND galactic_area_checkpoint galactic_production_tests --offer-projection)
add_test(NAME galactic_retained_census_contracts COMMAND galactic_area_checkpoint galactic_production_tests --retained-census)
add_test(NAME galactic_ground_sale_contracts COMMAND galactic_area_checkpoint galactic_production_tests --ground-sale)
add_test(NAME galactic_production_replay_corpus
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/replay_corpus_check.py"
        --program "$<TARGET_FILE:sim_headless>"
        --corpus "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/production-corpus")
