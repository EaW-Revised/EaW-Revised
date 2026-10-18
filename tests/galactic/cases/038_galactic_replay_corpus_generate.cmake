eawr_add_test_case(
    NAME galactic_replay_corpus_generate
    SOURCES replay_corpus_generate.cpp
    LIBS eawr::campaign
)

add_test(NAME galactic_replay_corpus
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/replay_corpus_check.py"
        --program "$<TARGET_FILE:sim_headless>"
        --corpus "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/corpus"
)
add_test(NAME galactic_replay_corpus_contracts
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_replay_corpus.py"
        --program "$<TARGET_FILE:sim_headless>"
        --corpus "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/corpus"
)

add_test(NAME gc_bench_worker_equality COMMAND gc_bench --ticks 60 --warmup 0)
add_test(NAME gc_bench_work_budgets COMMAND gc_bench --budgets --ticks 60 --warmup 0 --workers 1,2,4,8)
