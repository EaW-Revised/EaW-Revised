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
# Worker sweep of the same profile (PF mt-2): AI on, 1/2/4/8/hardware workers, 300 ticks (one slow-cadence cycle). The
# arguments replace the EAWR_TICK_PROFILE_* environment a pool job cannot set; the test fails when the tick hashes differ
# between worker counts. Run it by name (-R galactic_ai_tick_profile_sweep); batch-only keeps it out of routine runs.
eawr_add_game_data_test(NAME galactic_ai_tick_profile_sweep COMMAND galactic_ai_campaign_tests --tick-profile
    "${CMAKE_CURRENT_BINARY_DIR}/tick-profile-sweep" 300 on 1,2,4,8,hw)
set_tests_properties(galactic_ai_tick_profile_sweep PROPERTIES LABELS "game-data;benchmark;batch-only" TIMEOUT 28800
    RUN_SERIAL TRUE ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
# Long AI-on run past the first AI invasion of the human seat (#5518, tick ~10484 on the stock Equal Footing campaign). The
# harness answers the ready conflict choice like a player and fails by name if the galaxy waits on any other input.
eawr_add_game_data_test(NAME galactic_ai_tick_profile_human_encounter COMMAND galactic_ai_campaign_tests --tick-profile
    "${CMAKE_CURRENT_BINARY_DIR}/tick-profile-human-encounter" 10500 on 1)
set_tests_properties(galactic_ai_tick_profile_human_encounter PROPERTIES LABELS "game-data;benchmark;batch-only" TIMEOUT 7200
    RUN_SERIAL TRUE ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
eawr_add_game_data_test(NAME galactic_ai_campaign_preparation_stock COMMAND galactic_ai_campaign_tests --installed)
eawr_add_game_data_test(NAME galactic_ai_live_production_stock COMMAND galactic_ai_campaign_tests --live-production
    "${CMAKE_CURRENT_BINARY_DIR}/live-production" 1800)
eawr_add_game_data_test(NAME galactic_ai_live_performance_stock COMMAND galactic_ai_campaign_tests --live-performance
    "${CMAKE_CURRENT_BINARY_DIR}/live-performance" 30)
set_tests_properties(galactic_ai_live_production_stock galactic_ai_live_performance_stock PROPERTIES
    LABELS "game-data" TIMEOUT 28800 RUN_SERIAL TRUE ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
set_tests_properties(galactic_ai_campaign_runtime_contracts PROPERTIES ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
set_tests_properties(galactic_ai_campaign_preparation_stock PROPERTIES LABELS game-data ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
# WGM-38/163: the prepared AI game keeps the stock droid team's authored stealth before its first frame.
eawr_add_game_data_test(NAME galactic_ai_prepared_stealth_stock COMMAND galactic_ai_campaign_tests --prepared-stealth
    "${CMAKE_CURRENT_BINARY_DIR}/prepared-stealth")
set_tests_properties(galactic_ai_prepared_stealth_stock PROPERTIES LABELS game-data TIMEOUT 1800 RUN_SERIAL TRUE
    ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
# WGA-436..439: installed stock free-store units actually move (movement counter, in-transit witness,
# no unbound refusal) with equal state hashes at every determinism worker count.
eawr_add_game_data_test(NAME galactic_ai_freestore_movement_stock COMMAND galactic_ai_campaign_tests --freestore-movement
    "${CMAKE_CURRENT_BINARY_DIR}/freestore-movement" 1800)
set_tests_properties(galactic_ai_freestore_movement_stock PROPERTIES LABELS game-data TIMEOUT 7200 RUN_SERIAL TRUE
    ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
eawr_add_game_data_test(NAME galactic_ai_campaign_release_stock COMMAND galactic_ai_campaign_tests --release "${CMAKE_CURRENT_BINARY_DIR}/campaign-gate-correctness")
eawr_add_game_data_test(NAME galactic_ai_campaign_digest_mature_stock COMMAND galactic_ai_campaign_tests --release-benchmark "${CMAKE_CURRENT_BINARY_DIR}/campaign-gate-benchmark")
eawr_add_game_data_test(NAME galactic_ai_hash_baseline_diagnostic COMMAND galactic_ai_campaign_tests --hash-baseline "${CMAKE_CURRENT_BINARY_DIR}/hash-baseline")
set_tests_properties(galactic_ai_hash_baseline_diagnostic PROPERTIES LABELS "game-data;benchmark;batch-only" TIMEOUT 7200 RUN_SERIAL TRUE
    ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
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

# Async-AI quality sweep cases (--quality-run; tools/gc_quality). Registered only on request, see cmake/EawrGcQuality.cmake.
set(eawr_gc_quality_matrix "${PROJECT_SOURCE_DIR}/tools/gc_quality/sweep_matrix.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${eawr_gc_quality_matrix}")
if(EAWR_GC_QUALITY_SWEEP OR EXISTS "${PROJECT_SOURCE_DIR}/tools/gc_quality/sweep.enabled")
  include(EawrGcQuality)
  eawr_register_gc_quality_cases("${eawr_gc_quality_matrix}" galactic_ai_campaign_tests)
endif()
# #5519: the lockstep exchange encoding equals the serial canonical encoding on the installed stock campaign.
eawr_add_game_data_test(NAME galactic_exchange_encode_stock COMMAND galactic_ai_campaign_tests --exchange-encode 300)
set_tests_properties(galactic_exchange_encode_stock PROPERTIES LABELS game-data TIMEOUT 1800 RUN_SERIAL TRUE
    ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
# R1 (co-op design) research probe, not a PR gate: installed single-player map, one human team vs AI, multiplayer vs
# single-player victory rules. Opt-in: run by name (-R galactic_coop_victory_r1); batch-only keeps it out of routine runs.
eawr_add_game_data_test(NAME galactic_coop_victory_r1 COMMAND galactic_ai_campaign_tests --coop-victory-r1 1800)
set_tests_properties(galactic_coop_victory_r1 PROPERTIES LABELS "game-data;batch-only" TIMEOUT 3600 RUN_SERIAL TRUE ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")

# Steady-state AI cost per phase in Republic at War (#5489): AI off and on, 8 workers (the supported floor), 18000 ticks
# (10 game minutes); the statistics and the process CPU % of the logical processors cover minutes 5-10 (ticks 9000+).
# Skips (77) on a host without the workshop mod. Run by name (-R galactic_ai_tick_profile_raw_steady).
eawr_add_game_data_test(NAME galactic_ai_tick_profile_raw_steady COMMAND galactic_ai_campaign_tests --tick-profile
    "${CMAKE_CURRENT_BINARY_DIR}/tick-profile-raw-steady" 18000 off,on 8)
set_tests_properties(galactic_ai_tick_profile_raw_steady PROPERTIES LABELS "game-data;benchmark;batch-only" TIMEOUT 14400
    RUN_SERIAL TRUE SKIP_RETURN_CODE 77
    ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0;EAWR_TICK_PROFILE_MOD_ID=1129810972;EAWR_TICK_PROFILE_CAMPAIGN=Sandbox_The_Clone_Wars_Rebel;EAWR_TICK_PROFILE_STATS_FROM=9000")
