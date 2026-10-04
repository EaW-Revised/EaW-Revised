find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

add_executable(upgrade_identity_tests upgrade_identity_tests.cpp)
target_link_libraries(upgrade_identity_tests PRIVATE eawr::skirmish)
target_compile_features(upgrade_identity_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(upgrade_identity_tests)
add_test(NAME upgrade_identity_contracts COMMAND upgrade_identity_tests)
add_test(NAME upgrade_identity_stock_contracts COMMAND upgrade_identity_tests --stock)
set_tests_properties(upgrade_identity_stock_contracts PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")

set(EAWR_SKIRMISH_FIXTURES "${CMAKE_CURRENT_LIST_DIR}/fixtures")

add_executable(skirmish_start_tests skirmish_start_tests.cpp
    skirmish_start_synthetic_tests.cpp
    skirmish_start_foc_tests.cpp
    skirmish_start_pad_tests.cpp)
target_link_libraries(skirmish_start_tests PRIVATE eawr::skirmish eawr::platform)
target_compile_features(skirmish_start_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(skirmish_start_tests)
add_test(NAME skirmish_start_contracts COMMAND skirmish_start_tests "${EAWR_SKIRMISH_FIXTURES}")
add_test(
    NAME sim_headless_skirmish_contract
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_skirmish_cli.py"
        --program "$<TARGET_FILE:sim_headless>"
        --fixtures "${EAWR_SKIRMISH_FIXTURES}"
)

# The FoC tactical AI battle (#79): needs EAWR_EAW_GAME_ROOT, skipped otherwise.
add_executable(foc_ai_tests foc_ai_tests.cpp)
target_link_libraries(foc_ai_tests PRIVATE eawr::skirmish eawr::platform)
target_compile_features(foc_ai_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(foc_ai_tests)
add_test(NAME foc_ai_battle COMMAND foc_ai_tests)
set_tests_properties(foc_ai_battle PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")

# The FoC goal system and space plans in the M2 battle (#449): needs EAWR_EAW_GAME_ROOT.
add_executable(foc_plan_tests foc_plan_tests.cpp
    foc_plan_cases.cpp)
target_link_libraries(foc_plan_tests PRIVATE eawr::skirmish eawr::platform)
target_compile_features(foc_plan_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(foc_plan_tests)
add_test(NAME foc_plan_battle COMMAND foc_plan_tests)
set_tests_properties(foc_plan_battle PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME foc_ai_economy COMMAND foc_plan_tests 12000 "" "" --economy)
set_tests_properties(foc_ai_economy PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME foc_ai_underworld COMMAND foc_plan_tests 12000 "" "" --underworld)
set_tests_properties(foc_ai_underworld PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")

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
