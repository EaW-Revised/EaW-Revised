find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

add_executable(upgrade_identity_tests upgrade_identity_tests.cpp)
target_link_libraries(upgrade_identity_tests PRIVATE eawr::skirmish)
target_compile_features(upgrade_identity_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(upgrade_identity_tests)
add_test(NAME upgrade_identity_contracts COMMAND upgrade_identity_tests)
add_test(NAME upgrade_identity_stock_contracts COMMAND upgrade_identity_tests --stock)
set_tests_properties(upgrade_identity_stock_contracts PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")

# MAT-01 to MAT-10: the demo attract's settings, cycle, clocks, keys and saved camera (no game data).
add_executable(attract_tests attract_tests.cpp)
target_link_libraries(attract_tests PRIVATE eawr::skirmish)
target_compile_features(attract_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(attract_tests)
add_test(NAME skirmish_attract_contracts COMMAND attract_tests)

# MAT-15, MAT-16: the stock demo map's battle keeps going on the installed game (needs EAWR_EAW_GAME_ROOT).
add_executable(attract_lively_tests attract_lively_tests.cpp)
target_link_libraries(attract_lively_tests PRIVATE eawr::skirmish eawr::platform)
target_compile_features(attract_lively_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(attract_lively_tests)
add_test(NAME skirmish_attract_lively COMMAND attract_lively_tests 9000 1 2 4 8)
set_tests_properties(skirmish_attract_lively PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:" TIMEOUT 3600)
add_test(NAME skirmish_attract_cycle COMMAND attract_lively_tests 36000 8)
set_tests_properties(skirmish_attract_cycle PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:" TIMEOUT 3600
    LABELS "long;batch-only")

set(EAWR_SKIRMISH_FIXTURES "${CMAKE_CURRENT_LIST_DIR}/fixtures")

add_executable(skirmish_start_tests skirmish_start_tests.cpp
    skirmish_start_synthetic_tests.cpp
    skirmish_start_placement_tests.cpp
    skirmish_setup_contract_tests.cpp
    skirmish_start_placed_tests.cpp
    skirmish_start_foc_tests.cpp
    skirmish_start_pad_tests.cpp)
target_link_libraries(skirmish_start_tests PRIVATE eawr::skirmish eawr::platform)
target_compile_features(skirmish_start_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(skirmish_start_tests)
add_test(NAME skirmish_start_contracts COMMAND skirmish_start_tests "${EAWR_SKIRMISH_FIXTURES}")
eawr_add_game_data_test(NAME skirmish_start_installed_contracts
    COMMAND skirmish_start_tests "${EAWR_SKIRMISH_FIXTURES}" --installed)
add_test(
    NAME sim_headless_skirmish_contract
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_skirmish_cli.py"
        --program "$<TARGET_FILE:sim_headless>"
        --fixtures "${EAWR_SKIRMISH_FIXTURES}"
)
eawr_add_game_data_test(NAME sim_headless_skirmish_installed_contract
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_skirmish_cli.py"
        --program "$<TARGET_FILE:sim_headless>"
        --fixtures "${EAWR_SKIRMISH_FIXTURES}" --installed)

# The FoC tactical AI battle (#79): needs EAWR_EAW_GAME_ROOT, skipped otherwise.
add_executable(foc_ai_tests foc_ai_tests.cpp
    foc_ai_content_tests.cpp
    foc_ai_runner_tests.cpp
    foc_ai_probe_tests.cpp
    foc_ai_profile_tests.cpp)
target_link_libraries(foc_ai_tests PRIVATE eawr::skirmish eawr::platform)
target_compile_features(foc_ai_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(foc_ai_tests)
add_test(NAME foc_ai_battle COMMAND foc_ai_tests)
eawr_add_game_data_test(NAME foc_capital_avoidance COMMAND foc_ai_tests --capital-avoidance)
set_tests_properties(foc_capital_avoidance PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:" TIMEOUT 900)
# GFS-A24 release acceptance runs BOTH the pure identity profile and the restore row below.
eawr_add_game_data_test(NAME foc_ai_player_stream_profile COMMAND foc_ai_tests --player-stream-profile 5400)
set_tests_properties(foc_ai_player_stream_profile PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:"
    LABELS "game-data;opt-in;batch-only" TIMEOUT 3600 RUN_SERIAL TRUE)
eawr_add_game_data_test(NAME foc_ai_player_stream_reference COMMAND foc_ai_tests --player-stream-profile 5400 1)
set_tests_properties(foc_ai_player_stream_reference PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:"
    LABELS "game-data;opt-in;batch-only" TIMEOUT 1800 RUN_SERIAL TRUE)
eawr_add_game_data_test(NAME foc_ai_player_stream_restore COMMAND foc_ai_tests --player-stream-restore 5400)
set_tests_properties(foc_ai_player_stream_restore PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:"
    LABELS "game-data;opt-in;batch-only" TIMEOUT 1800 RUN_SERIAL TRUE)
eawr_add_game_data_test(NAME foc_ai_serial_profile COMMAND foc_ai_tests --serial-profile 5400)
set_tests_properties(foc_ai_serial_profile PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:"
    LABELS "game-data;performance;opt-in;batch-only" TIMEOUT 3600)
eawr_add_game_data_test(NAME foc_ai_serial_breakdown COMMAND foc_ai_tests --serial-profile 5400 1 6)
set_tests_properties(foc_ai_serial_breakdown PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:"
    LABELS "game-data;performance;opt-in;batch-only" TIMEOUT 1800)
add_test(NAME foc_ai_squadron_idle_trace COMMAND foc_ai_tests --squadron-idle-trace 3600)
set_tests_properties(foc_ai_squadron_idle_trace PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:" TIMEOUT 2520)
# Wall budget rounds 1.5 * 1660.26s measured whole-case time up to minutes.
set_tests_properties(foc_ai_battle PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:" TIMEOUT 2520 RUN_SERIAL TRUE)
add_test(NAME foc_ai_stock_economy_witness COMMAND foc_ai_tests --stock-economy-witness)
# Wall budget rounds 1.5 * 1027.5s measured whole-case time up to minutes.
set_tests_properties(foc_ai_stock_economy_witness PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:"
    LABELS "long;stock-economy" TIMEOUT 1560)
# WAB-70..76: the Underworld AI fires BUZZ_DROIDS from its StarVipers through the Lua calls (needs EAWR_EAW_GAME_ROOT).
add_test(NAME foc_ai_underworld_buzz COMMAND foc_ai_tests --underworld 3600)
set_tests_properties(foc_ai_underworld_buzz PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
# EX-46: under the stock Underworld plans alone, the plans' Target_In_Range handler activates BUZZ_DROIDS (needs EAWR_EAW_GAME_ROOT).
add_test(NAME foc_ai_underworld_target_in_range COMMAND foc_ai_tests --underworld-stock 9000)
set_tests_properties(foc_ai_underworld_target_in_range PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
# WAB-80..83: the Underworld AI switches FULL_SALVO on for its Interceptor IV frigates through the Lua calls (needs EAWR_EAW_GAME_ROOT).
add_test(NAME foc_ai_underworld_salvo COMMAND foc_ai_tests --interceptor 3600)
set_tests_properties(foc_ai_underworld_salvo PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
# WSD-01..09: the Underworld AI's Vengeance frigates self-destruct through the Lua calls (needs EAWR_EAW_GAME_ROOT).
add_test(NAME foc_ai_underworld_selfdestruct COMMAND foc_ai_tests --vengeance 5400)
set_tests_properties(foc_ai_underworld_selfdestruct PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
# WST-16: the Underworld AI against a cloaking Vengeance stays stealth-blind and moves on (needs EAWR_EAW_GAME_ROOT).
add_test(NAME foc_ai_cloaked_enemy COMMAND foc_ai_tests --cloaked 2400)
set_tests_properties(foc_ai_cloaked_enemy PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME foc_ai_underworld_leech COMMAND foc_ai_tests --keldabe 1800)
set_tests_properties(foc_ai_underworld_leech PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
# WLU-10: the Underworld AI's A-wings switch LURE on through the Lua calls (needs EAWR_EAW_GAME_ROOT).
add_test(NAME foc_ai_underworld_lure COMMAND foc_ai_tests --awing 3600)
set_tests_properties(foc_ai_underworld_lure PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
# WHE-33/67: IG-2000's native object script casts in an installed-data battle on 1/2/4/8 workers.
add_test(NAME foc_ai_underworld_corrupt COMMAND foc_ai_tests --ig2000 450)
set_tests_properties(foc_ai_underworld_corrupt PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
# WAB-36: under the stock plans the AI Keldabe autofires LEECH_SHIELDS onto its attack target.
add_test(NAME foc_ai_underworld_leech_autofire COMMAND foc_ai_tests --keldabe-autofire 1800)
set_tests_properties(foc_ai_underworld_leech_autofire PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
# WAB-36: under the stock plans the AI Star Destroyer autofires TRACTOR_BEAM onto its attack target.
add_test(NAME foc_ai_tractor_autofire COMMAND foc_ai_tests --tractor 1800)
set_tests_properties(foc_ai_tractor_autofire PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
# WAB-36, WHE-24: under the stock plans the AI Home One autofires CONCENTRATE_FIRE; its escorts fire on the target.
add_test(NAME foc_ai_concentrate_autofire COMMAND foc_ai_tests --concentrate 1800)
set_tests_properties(foc_ai_concentrate_autofire PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")

# The FoC goal system and space plans in the M2 battle (#449): needs EAWR_EAW_GAME_ROOT.
add_executable(foc_plan_tests foc_plan_tests.cpp
    foc_plan_cases.cpp)
target_link_libraries(foc_plan_tests PRIVATE eawr::skirmish eawr::platform)
target_compile_features(foc_plan_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(foc_plan_tests)
# WGS-66: installed stock Engine/Freestore continuation; supplied content is validated.
eawr_add_game_data_test(NAME foc_tactical_native_continuation COMMAND foc_plan_tests --native-continuation)
set_tests_properties(foc_tactical_native_continuation PROPERTIES TIMEOUT 600 ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
set_property(TEST foc_tactical_native_continuation APPEND PROPERTY LABELS game-data)
add_test(NAME foc_plan_battle COMMAND foc_plan_tests)
# Wall budget rounds 1.5 * 3210.46s measured whole-case time up to minutes.
set_tests_properties(foc_plan_battle PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:" TIMEOUT 4860)
# PL-01/02/03: seed 3 is an explicit long-tail counterexample to the per-PR
# 18000-tick cohort: first start 29837, first order 29854 in a 36000-tick probe.
# Keep a 600-tick project margin; wall budget rounds 1.5 * 486.267s up to minutes.
add_test(NAME foc_plan_long_tail COMMAND foc_plan_tests --attack-long-tail 30437)
set_tests_properties(foc_plan_long_tail PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:"
    LABELS "long;batch-only" TIMEOUT 780)
add_test(NAME foc_ai_economy COMMAND foc_plan_tests 12000 "" "" --economy)
set_tests_properties(foc_ai_economy PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME foc_ai_reinforce_position COMMAND foc_plan_tests 3600
    "${CMAKE_CURRENT_BINARY_DIR}/reinforce-position.eawr-replay" "" --reinforce-regression)
set_tests_properties(foc_ai_reinforce_position PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME foc_ai_underworld COMMAND foc_plan_tests 12000 "" "" --underworld)
set_tests_properties(foc_ai_underworld PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
foreach(faction Rebel Empire Underworld)
    add_test(NAME foc_ai_special_weapon_${faction} COMMAND foc_plan_tests --special-weapons ${faction} 18000)
    set_tests_properties(foc_ai_special_weapon_${faction} PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
endforeach()

# The Empire AI's burn attack on the starbase after the Rebel fleet is gone (#532): needs EAWR_EAW_GAME_ROOT.
add_executable(foc_burn_tests foc_burn_tests.cpp)
target_link_libraries(foc_burn_tests PRIVATE eawr::skirmish eawr::platform)
target_compile_features(foc_burn_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(foc_burn_tests)
add_test(NAME foc_burn_battle COMMAND foc_burn_tests)
set_tests_properties(foc_burn_battle PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")

# The owner's path benchmark (#520): the 20-ship order's searches pinned by their work and the
# per-tick search budget held (space-movement PC-07); prints the tick times. Needs
# EAWR_EAW_GAME_ROOT, skipped otherwise.
add_test(NAME path_bench_owner_work
    COMMAND path_bench --selection owner --workers 1,4 --timing off
        --pin "${EAWR_SKIRMISH_FIXTURES}/path-bench-owner.work.csv")
set_tests_properties(path_bench_owner_work PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")

# The close-range battle benchmark (#601) at size S, trimmed to its first 600 ticks (the fighters
# engage, the hangars launch and the deaths start): every tick's state hash must agree across 1, 2, 4
# and 8 workers. Needs EAWR_EAW_GAME_ROOT, skipped otherwise.
add_test(NAME path_bench_melee_workers
    COMMAND path_bench --melee s --ticks 600 --workers 1,2,4,8 --timing off)
set_tests_properties(path_bench_melee_workers PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")

# The soak's invariants and command line (#627), tested without the game.
add_executable(ai_progression_probe ai_progression_probe.cpp)
target_link_libraries(ai_progression_probe PRIVATE eawr::skirmish eawr::platform)
target_compile_features(ai_progression_probe PRIVATE cxx_std_20)
eawr_target_project_warnings(ai_progression_probe)
eawr_add_game_data_test(NAME foc_ai_station_pace
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/station_pace_check.py"
        "$<TARGET_FILE:ai_progression_probe>" "${CMAKE_CURRENT_BINARY_DIR}/station-pace")
set_tests_properties(foc_ai_station_pace PROPERTIES
    SKIP_REGULAR_EXPRESSION "SKIPPED:" LABELS "game-data" TIMEOUT 1800)
# Coordinator-defined #3716 acceptance remains outside CTest/default release
# suites while the timing gap is open. This target is deliberately not ALL.
add_custom_target(foc_ai_extractor_lifetime
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_LIST_DIR}/extractor_lifetime_check.py"
        "${CMAKE_CURRENT_BINARY_DIR}/extractor_lifetime_check.py"
    DEPENDS ai_progression_probe
    COMMENT "Prepare the manual #3716 acceptance runner (execute separately with an installed root)"
    VERBATIM)
# #1933: 60000-tick AI-vs-AI duels on the live skirmish economy with the full stock menu, logging
# superweapon purchase, permission, readiness and population room. Observation only; outputs under
# the build tree's superweapon-long/ folder.
foreach(duel Rebel:Empire Underworld:Empire Underworld:Rebel)
    string(REPLACE ":" "_" duel_name "${duel}")
    add_test(NAME foc_ai_superweapon_long_${duel_name}
        COMMAND ai_progression_probe env data/art/maps/_mp_space_coruscant.ted Normal_Default 60000 1
            "${CMAKE_CURRENT_BINARY_DIR}/superweapon-long/${duel_name}" duel:${duel})
    set_tests_properties(foc_ai_superweapon_long_${duel_name} PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:"
        LABELS "long;batch-only" TIMEOUT 1800)
endforeach()

add_library(eawr_soak STATIC soak_invariants.cpp soak_options.cpp)
target_link_libraries(eawr_soak PUBLIC eawr::sim)
target_compile_features(eawr_soak PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_soak)
add_executable(soak_unit_tests soak_unit_tests.cpp)
target_link_libraries(soak_unit_tests PRIVATE eawr_soak)
eawr_target_project_warnings(soak_unit_tests)
add_test(NAME soak_contracts COMMAND soak_unit_tests)

# The M2 battle with the AI on both sides, shared by the soak and the tag perturbation check.
add_library(eawr_m2_battle STATIC m2_battle.cpp)
target_link_libraries(eawr_m2_battle PUBLIC eawr::skirmish)
target_compile_features(eawr_m2_battle PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_m2_battle)

# Long FoC-AI battles with the AI on both sides (#615, #627): needs EAWR_EAW_GAME_ROOT. Seed 12 hit
# EAWR-SIM-0310 in FD-10's look-ahead share at tick 2469 before the guard; it must run past it. The
# invariants are the nightly soak's (tools/soak, docs/nightly-soak.md), not a merge gate, so this
# test runs without them.
add_executable(foc_soak_tests foc_soak_tests.cpp)
target_link_libraries(foc_soak_tests PRIVATE eawr::skirmish eawr::platform eawr_soak eawr_m2_battle)
target_compile_features(foc_soak_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(foc_soak_tests)
add_test(NAME foc_journal_overflow COMMAND foc_soak_tests --journal-regression)
add_test(NAME foc_soak_615 COMMAND foc_soak_tests --seeds 12 --ticks 2500 --no-invariants)
set_tests_properties(foc_soak_615 PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
# #664 (space-fighters FM-20): an idle leader that sat on its hold point let a wingman run away. On
# the #605 start, seed 31 moves a craft at 40 times its maximum speed at tick 4391 without the fix;
# with it the speed and map invariants hold through tick 4500. Slot crowding and hull overlap are
# timed out here: they are the idle grid's (#687) and the ships'.
add_test(NAME foc_soak_664 COMMAND foc_soak_tests --seeds 31 --ticks 4500 --slot-ticks 1000000 --hull-ticks 1000000)
set_tests_properties(foc_soak_664 PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")

# The AI's scheduled work per tick (#957, SCH-04): counted from the goal system's journal, not timed.
# Under the staggered schedule no tick attaches more plans than its cap; the faithful schedule on the
# same seed does, so the check measures something. The M2
# start's first goal pass attaches four plans in one tick (about tick 380). Needs EAWR_EAW_GAME_ROOT.
add_test(NAME foc_schedule_957 COMMAND foc_soak_tests --seeds 1 --ticks 600 --no-invariants --check-schedule)
set_tests_properties(foc_schedule_957 PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")

# An AI ship with an enemy astern (#668): staged in the M2 battle, it keeps its heading until the
# AI orders it, and an attack order turns it at once. Needs EAWR_EAW_GAME_ROOT.
add_executable(foc_turn_tests foc_turn_tests.cpp)
target_link_libraries(foc_turn_tests PRIVATE eawr::skirmish eawr::platform)
target_compile_features(foc_turn_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(foc_turn_tests)
add_test(NAME foc_ai_turn_668 COMMAND foc_turn_tests)
set_tests_properties(foc_ai_turn_668 PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()


# The tag perturbation check (docs/tag-applied-check.md): a registry row's value changed in memory
# for one object type, the M2 battle run against the unchanged one. It runs nightly or on demand
# through tools/inventory/tag_applied_check.py, not as a merge gate.
add_executable(foc_tag_perturb
    foc_tag_perturb.cpp
    foc_tag_perturb_install.cpp
    foc_tag_perturb_run.cpp
    foc_tag_perturb_report.cpp
)
target_link_libraries(foc_tag_perturb PRIVATE eawr::skirmish eawr::platform eawr_m2_battle)
target_compile_features(foc_tag_perturb PRIVATE cxx_std_20)
eawr_target_project_warnings(foc_tag_perturb)

# Installed combat/motion replays are batch/release coverage, never a routine PR gate.
eawr_add_game_data_test(NAME installed_replay_recording
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/replay/record_installed.py"
        --program "$<TARGET_FILE:sim_headless>"
        --scenarios "${EAWR_SKIRMISH_FIXTURES}/installed"
        --out "${CMAKE_BINARY_DIR}/installed-replays" --check)
set_tests_properties(installed_replay_recording PROPERTIES
    LABELS "batch-only;installed-required;game-data" TIMEOUT 2400)
