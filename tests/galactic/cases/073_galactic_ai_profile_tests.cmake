eawr_add_test_case(
    NAME galactic_ai_profile_tests
    SOURCES gc_profile_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/include"
    TESTS galactic_ai_profile_contracts
)

if(WIN32)
  eawr_add_game_data_test(NAME galactic_ai_profile_retail600 COMMAND galactic_ai_campaign_tests --live-profile-comparison
    "${CMAKE_CURRENT_BINARY_DIR}/live-profile" 600)
  set_tests_properties(galactic_ai_profile_retail600 PROPERTIES LABELS "game-data"
    TIMEOUT 14400 RUN_SERIAL TRUE ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
endif()
# Live galactic tick cost per phase, AI off and on (#4049). Output directory, ticks, modes and worker counts come from
# EAWR_TICK_PROFILE_DIR/_TICKS/_MODES/_WORKERS; the registered run is a 30-tick smoke.
eawr_add_game_data_test(NAME galactic_ai_tick_profile COMMAND galactic_ai_campaign_tests --tick-profile
    "${CMAKE_CURRENT_BINARY_DIR}/tick-profile" 30)
set_tests_properties(galactic_ai_tick_profile PROPERTIES LABELS "game-data" TIMEOUT 28800 RUN_SERIAL TRUE
    ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
eawr_add_game_data_test(NAME galactic_ai_campaign_preparation_stock COMMAND galactic_ai_campaign_tests --installed)
eawr_add_game_data_test(NAME galactic_ai_live_production_stock COMMAND galactic_ai_campaign_tests --live-production
    "${CMAKE_CURRENT_BINARY_DIR}/live-production" 1800)
eawr_add_game_data_test(NAME galactic_ai_live_performance_stock COMMAND galactic_ai_campaign_tests --live-performance
    "${CMAKE_CURRENT_BINARY_DIR}/live-performance" 30)
set_tests_properties(galactic_ai_live_production_stock galactic_ai_live_performance_stock PROPERTIES
    LABELS "game-data" TIMEOUT 28800 RUN_SERIAL TRUE ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
set_tests_properties(galactic_ai_campaign_runtime_contracts PROPERTIES ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
set_tests_properties(galactic_ai_campaign_preparation_stock PROPERTIES LABELS game-data ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
# WGA-436..439: installed stock free-store units actually move (movement counter, in-transit witness,
# no unbound refusal) with equal state hashes at every determinism worker count.
eawr_add_game_data_test(NAME galactic_ai_freestore_movement_stock COMMAND galactic_ai_campaign_tests --freestore-movement
    "${CMAKE_CURRENT_BINARY_DIR}/freestore-movement" 1800)
set_tests_properties(galactic_ai_freestore_movement_stock PROPERTIES LABELS game-data TIMEOUT 7200 RUN_SERIAL TRUE
    ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
eawr_add_game_data_test(NAME galactic_ai_campaign_release_stock COMMAND galactic_ai_campaign_tests --release "${CMAKE_CURRENT_BINARY_DIR}/campaign-gate-correctness")
eawr_add_game_data_test(NAME galactic_ai_campaign_digest_mature_stock COMMAND galactic_ai_campaign_tests --release-benchmark "${CMAKE_CURRENT_BINARY_DIR}/campaign-gate-benchmark")
set_tests_properties(galactic_ai_campaign_release_stock PROPERTIES LABELS "game-data;release-gate" ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
set_tests_properties(galactic_ai_campaign_digest_mature_stock PROPERTIES LABELS "game-data;release-gate;benchmark" ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
set_tests_properties(galactic_ai_campaign_release_stock galactic_ai_campaign_digest_mature_stock PROPERTIES TIMEOUT 7200 RUN_SERIAL TRUE)
# The 27,000-frame correctness gate can take about 2.5 hours with full hash
# verification; allow four hours without changing its assertions or frame limit.
set_tests_properties(galactic_ai_campaign_release_stock PROPERTIES TIMEOUT 14400)
eawr_add_game_data_test(NAME galactic_ai_campaign_policy_equality_stock COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/galactic/ai_campaign_policy_compare.py"
    "${CMAKE_CURRENT_BINARY_DIR}/campaign-gate-correctness" "${CMAKE_CURRENT_BINARY_DIR}/campaign-gate-benchmark")
set_tests_properties(galactic_ai_campaign_policy_equality_stock PROPERTIES LABELS "game-data;release-gate"
    SKIP_REGULAR_EXPRESSION "SKIPPED:"
    DEPENDS "galactic_ai_campaign_release_stock;galactic_ai_campaign_digest_mature_stock")
# --release is deliberately not a passing CTest registration while its actual
# gameplay/listener providers remain absent; the release owner runs it explicitly.
